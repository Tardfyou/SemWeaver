#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/Version.h"
#include "clang/Lex/Lexer.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/CheckerManager.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/MemRegion.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SVals.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include "llvm/ADT/StringRef.h"
#include <memory>

using namespace clang;
using namespace ento;

namespace {

static const Expr *ignoreParenImpCasts(const Expr *E) {
  if (!E)
    return nullptr;
  return E->IgnoreParenImpCasts();
}

static const Expr *stripCastsAndParens(const Expr *E) {
  const Expr *Cur = E;
  while (Cur) {
    const Expr *Next = Cur->IgnoreParenImpCasts();
    if (Next == Cur)
      break;
    Cur = Next;
  }
  return Cur;
}

static const ValueDecl *getBaseValueDecl(const Expr *E) {
  const Expr *Base = stripCastsAndParens(E);
  if (!Base)
    return nullptr;

  if (const auto *DRE = dyn_cast<DeclRefExpr>(Base))
    return DRE->getDecl();

  if (const auto *ME = dyn_cast<MemberExpr>(Base))
    return getBaseValueDecl(ME->getBase());

  if (const auto *UO = dyn_cast<UnaryOperator>(Base)) {
    if (UO->getOpcode() == UO_AddrOf || UO->getOpcode() == UO_Deref)
      return getBaseValueDecl(UO->getSubExpr());
  }

  if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(Base))
    return getBaseValueDecl(ASE->getBase());

  return nullptr;
}

static bool isSpecificDeclRef(const Expr *E, const ValueDecl *Target) {
  if (!E || !Target)
    return false;
  return getBaseValueDecl(E) == Target;
}

static bool isTupleAppendTvCall(const CallExpr *CE, const ValueDecl *Target) {
  if (!CE || !Target)
    return false;

  const FunctionDecl *FD = CE->getDirectCallee();
  if (!FD)
    return false;

  StringRef Name = FD->getName();
  if (Name != "tuple_append_tv")
    return false;

  if (CE->getNumArgs() < 2)
    return false;

  return isSpecificDeclRef(CE->getArg(1), Target);
}

static bool exprMentionsDecl(const Expr *E, const ValueDecl *Target) {
  if (!E || !Target)
    return false;

  const Expr *Base = stripCastsAndParens(E);
  if (!Base)
    return false;

  if (const auto *DRE = dyn_cast<DeclRefExpr>(Base))
    return DRE->getDecl() == Target;

  if (const auto *ME = dyn_cast<MemberExpr>(Base))
    return exprMentionsDecl(ME->getBase(), Target);

  if (const auto *UO = dyn_cast<UnaryOperator>(Base))
    return exprMentionsDecl(UO->getSubExpr(), Target);

  if (const auto *BO = dyn_cast<BinaryOperator>(Base))
    return exprMentionsDecl(BO->getLHS(), Target) || exprMentionsDecl(BO->getRHS(), Target);

  if (const auto *CO = dyn_cast<ConditionalOperator>(Base))
    return exprMentionsDecl(CO->getCond(), Target) ||
           exprMentionsDecl(CO->getTrueExpr(), Target) ||
           exprMentionsDecl(CO->getFalseExpr(), Target);

  if (const auto *CE = dyn_cast<CallExpr>(Base)) {
    for (const Expr *Arg : CE->arguments()) {
      if (exprMentionsDecl(Arg, Target))
        return true;
    }
    return false;
  }

  if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(Base))
    return exprMentionsDecl(ASE->getBase(), Target) || exprMentionsDecl(ASE->getIdx(), Target);

  return false;
}

static bool rhsIsVarUnknown(const Expr *E) {
  const Expr *Base = stripCastsAndParens(E);
  if (!Base)
    return false;

  if (const auto *DRE = dyn_cast<DeclRefExpr>(Base)) {
    const ValueDecl *VD = DRE->getDecl();
    if (!VD)
      return false;
    return VD->getName() == "VAR_UNKNOWN";
  }

  if (const auto *IL = dyn_cast<IntegerLiteral>(Base))
    return IL->getValue() == 0;

  return false;
}

static bool isRettvVTypeInvalidationStmt(const Stmt *S, const ValueDecl *Target) {
  if (!S || !Target)
    return false;

  const auto *BO = dyn_cast<BinaryOperator>(S);
  if (!BO || !BO->isAssignmentOp())
    return false;

  const Expr *LHS = stripCastsAndParens(BO->getLHS());
  const auto *ME = dyn_cast_or_null<MemberExpr>(LHS);
  if (!ME)
    return false;

  if (!exprMentionsDecl(ME->getBase(), Target))
    return false;

  const ValueDecl *Member = ME->getMemberDecl();
  if (!Member)
    return false;

  if (Member->getName() != "v_type")
    return false;

  return rhsIsVarUnknown(BO->getRHS());
}

static bool stmtContainsInvalidation(const Stmt *S, const ValueDecl *Target) {
  if (!S || !Target)
    return false;

  if (isRettvVTypeInvalidationStmt(S, Target))
    return true;

  for (const Stmt *Child : S->children()) {
    if (!Child)
      continue;
    if (stmtContainsInvalidation(Child, Target))
      return true;
  }
  return false;
}

class TypvalTransferWithoutInvalidationChecker
    : public Checker<check::ASTCodeBody> {
  mutable std::unique_ptr<BugType> BT;

public:
  TypvalTransferWithoutInvalidationChecker()
      : BT(std::make_unique<BugType>(this,
                                     "typval transfer without invalidation",
                                     "Memory Safety")) {}

  void checkASTCodeBody(const Decl *D, AnalysisManager &, BugReporter &BR) const;
};

class TransferVisitor : public RecursiveASTVisitor<TransferVisitor> {
  const TypvalTransferWithoutInvalidationChecker *Checker;
  BugReporter &BR;
  const SourceManager &SM;
  const FunctionDecl *CurrentFD;
  StringRef FileName;

public:
  TransferVisitor(const TypvalTransferWithoutInvalidationChecker *Checker,
                  BugReporter &BR,
                  const SourceManager &SM,
                  const FunctionDecl *FD,
                  StringRef FileName)
      : Checker(Checker), BR(BR), SM(SM), CurrentFD(FD), FileName(FileName) {}

  bool VisitCompoundStmt(CompoundStmt *CS) {
    if (!CS || !CurrentFD)
      return true;

    for (CompoundStmt::body_iterator I = CS->body_begin(), E = CS->body_end(); I != E; ++I) {
      Stmt *Cur = *I;
      if (!Cur)
        continue;

      const auto *CallS = dyn_cast<CallExpr>(Cur);
      if (!CallS)
        continue;

      const FunctionDecl *Callee = CallS->getDirectCallee();
      if (!Callee)
        continue;

      if (Callee->getName() != "tuple_append_tv")
        continue;

      if (CallS->getNumArgs() < 2)
        continue;

      const ValueDecl *Transferred = getBaseValueDecl(CallS->getArg(1));
      if (!Transferred)
        continue;

      bool HasInvalidation = false;
      auto J = I;
      ++J;
      for (; J != E; ++J) {
        Stmt *Next = *J;
        if (!Next)
          continue;
        if (stmtContainsInvalidation(Next, Transferred)) {
          HasInvalidation = true;
          break;
        }
      }

      if (HasInvalidation)
        continue;

      SourceLocation Loc = Cur->getBeginLoc();
      if (Loc.isInvalid())
        continue;

      PathDiagnosticLocation PLoc(Loc, SM);
      BR.EmitBasicReport(CurrentFD,
                         Checker,
                         "typval transferred to tuple without invalidation",
                         "Memory Safety",
                         "A typval is appended to a tuple via tuple_append_tv, but the source typval is not invalidated afterwards. The caller may still free it based on the stale v_type, causing a double free.",
                         PLoc,
                         Cur->getSourceRange());
    }

    return true;
  }
};

void TypvalTransferWithoutInvalidationChecker::checkASTCodeBody(
    const Decl *D, AnalysisManager &, BugReporter &BR) const {
  const auto *FD = dyn_cast<FunctionDecl>(D);
  if (!FD)
    return;

  const Stmt *Body = FD->getBody();
  if (!Body)
    return;

  SourceLocation FuncLoc = FD->getLocation();
  if (FuncLoc.isInvalid())
    return;

  const SourceManager &SM = BR.getSourceManager();
  StringRef Path = SM.getFilename(SM.getSpellingLoc(FuncLoc));
  if (Path.empty())
    return;

  if (!Path.ends_with("src/tuple.c"))
    return;

  TransferVisitor V(this, BR, SM, FD, Path);
  V.TraverseStmt(const_cast<Stmt *>(Body));
}

} // namespace

extern "C" void clang_registerCheckers(CheckerRegistry &Registry) {
  Registry.addChecker<TypvalTransferWithoutInvalidationChecker>(
      "custom.TypvalTransferWithoutInvalidationChecker",
      "Detect typval transfer to tuple without invalidating source typval.",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] = CLANG_VERSION_STRING;
