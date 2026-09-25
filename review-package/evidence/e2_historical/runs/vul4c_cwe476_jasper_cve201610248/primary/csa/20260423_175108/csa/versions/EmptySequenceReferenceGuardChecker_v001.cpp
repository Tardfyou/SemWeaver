#include "clang/AST/Expr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/Decl.h"
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

static const Expr *ignoreCastsAndParens(const Expr *E) {
  if (!E)
    return nullptr;
  return E->IgnoreParenImpCasts();
}

static const CallExpr *asNamedCall(const Expr *E, llvm::StringRef Name) {
  const Expr *Core = ignoreCastsAndParens(E);
  if (!Core)
    return nullptr;
  const auto *CE = dyn_cast<CallExpr>(Core);
  if (!CE)
    return nullptr;
  const FunctionDecl *FD = CE->getDirectCallee();
  if (!FD)
    return nullptr;
  StringRef CalleeName = FD->getName();
  if (CalleeName != Name)
    return nullptr;
  return CE;
}

static const ValueDecl *getReferencedDecl(const Expr *E) {
  const Expr *Core = ignoreCastsAndParens(E);
  if (!Core)
    return nullptr;
  const auto *DRE = dyn_cast<DeclRefExpr>(Core);
  if (!DRE)
    return nullptr;
  return DRE->getDecl();
}

static bool exprReferencesDecl(const Expr *E, const ValueDecl *Target) {
  if (!E || !Target)
    return false;
  E = ignoreCastsAndParens(E);
  if (!E)
    return false;

  if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
    return DRE->getDecl() == Target;

  if (const auto *ME = dyn_cast<MemberExpr>(E))
    return exprReferencesDecl(ME->getBase(), Target);

  if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(E))
    return exprReferencesDecl(ASE->getBase(), Target) ||
           exprReferencesDecl(ASE->getIdx(), Target);

  if (const auto *UO = dyn_cast<UnaryOperator>(E))
    return exprReferencesDecl(UO->getSubExpr(), Target);

  if (const auto *BO = dyn_cast<BinaryOperator>(E))
    return exprReferencesDecl(BO->getLHS(), Target) ||
           exprReferencesDecl(BO->getRHS(), Target);

  if (const auto *CO = dyn_cast<ConditionalOperator>(E))
    return exprReferencesDecl(CO->getCond(), Target) ||
           exprReferencesDecl(CO->getTrueExpr(), Target) ||
           exprReferencesDecl(CO->getFalseExpr(), Target);

  if (const auto *CE = dyn_cast<CallExpr>(E)) {
    for (const Expr *Arg : CE->arguments()) {
      if (exprReferencesDecl(Arg, Target))
        return true;
    }
    return false;
  }

  for (const Stmt *Child : E->children()) {
    if (!Child)
      continue;
    if (const auto *ChildExpr = dyn_cast<Expr>(Child)) {
      if (exprReferencesDecl(ChildExpr, Target))
        return true;
    }
  }
  return false;
}

static bool containsPositiveNumlvlsCheck(const Expr *E, const ValueDecl *TsfbParam) {
  E = ignoreCastsAndParens(E);
  if (!E)
    return false;

  if (const auto *BO = dyn_cast<BinaryOperator>(E)) {
    if (BO->getOpcode() == BO_GT || BO->getOpcode() == BO_GE) {
      const Expr *LHS = ignoreCastsAndParens(BO->getLHS());
      const Expr *RHS = ignoreCastsAndParens(BO->getRHS());
      if (LHS && RHS && isa<IntegerLiteral>(RHS)) {
        const auto *ME = dyn_cast<MemberExpr>(LHS);
        const auto *IL = dyn_cast<IntegerLiteral>(RHS);
        if (ME && IL && exprReferencesDecl(ME->getBase(), TsfbParam)) {
          const ValueDecl *Member = ME->getMemberDecl();
          if (Member && Member->getName() == "numlvls") {
            llvm::APSInt V = IL->getValue();
            if ((BO->getOpcode() == BO_GT && V == 0) ||
                (BO->getOpcode() == BO_GE && V == 1))
              return true;
          }
        }
      }
    }

    if (BO->isLogicalOp()) {
      return containsPositiveNumlvlsCheck(BO->getLHS(), TsfbParam) ||
             containsPositiveNumlvlsCheck(BO->getRHS(), TsfbParam);
    }
  }

  if (const auto *CO = dyn_cast<ConditionalOperator>(E))
    return containsPositiveNumlvlsCheck(CO->getCond(), TsfbParam);

  return false;
}

static bool containsSeqSizeGuard(const Expr *E, const ValueDecl *SeqParam) {
  E = ignoreCastsAndParens(E);
  if (!E || !SeqParam)
    return false;

  if (const auto *CE = asNamedCall(E, "jas_seq2d_size")) {
    if (CE->getNumArgs() >= 1 && exprReferencesDecl(CE->getArg(0), SeqParam))
      return true;
  }

  if (const auto *BO = dyn_cast<BinaryOperator>(E)) {
    return containsSeqSizeGuard(BO->getLHS(), SeqParam) ||
           containsSeqSizeGuard(BO->getRHS(), SeqParam);
  }

  if (const auto *CO = dyn_cast<ConditionalOperator>(E)) {
    return containsSeqSizeGuard(CO->getCond(), SeqParam) ||
           containsSeqSizeGuard(CO->getTrueExpr(), SeqParam) ||
           containsSeqSizeGuard(CO->getFalseExpr(), SeqParam);
  }

  for (const Stmt *Child : E->children()) {
    if (!Child)
      continue;
    if (const auto *ChildExpr = dyn_cast<Expr>(Child)) {
      if (containsSeqSizeGuard(ChildExpr, SeqParam))
        return true;
    }
  }
  return false;
}

class EmptySequenceReferenceGuardChecker
    : public Checker<check::ASTCodeBody> {
  mutable std::unique_ptr<BugType> BT;

public:
  EmptySequenceReferenceGuardChecker()
      : BT(std::make_unique<BugType>(this, "empty sequence reference without size guard",
                                     "Custom")) {}

  void checkASTCodeBody(const Decl *D, AnalysisManager &, BugReporter &BR) const;
};

class BodyVisitor : public RecursiveASTVisitor<BodyVisitor> {
  BugReporter &BR;
  const CheckerBase *Checker;
  const BugType &BT;
  const FunctionDecl *FD;
  const ValueDecl *TsfbParam;
  const ValueDecl *SeqParam;
  bool Reported = false;

public:
  BodyVisitor(BugReporter &BR, const CheckerBase *Checker, const BugType &BT,
              const FunctionDecl *FD, const ValueDecl *TsfbParam,
              const ValueDecl *SeqParam)
      : BR(BR), Checker(Checker), BT(BT), FD(FD), TsfbParam(TsfbParam),
        SeqParam(SeqParam) {}

  bool VisitReturnStmt(ReturnStmt *RS) {
    if (Reported || !RS || !FD || !SeqParam)
      return true;

    const Expr *RetValue = RS->getRetValue();
    RetValue = ignoreCastsAndParens(RetValue);
    if (!RetValue)
      return true;

    const auto *CO = dyn_cast<ConditionalOperator>(RetValue);
    if (!CO)
      return true;

    const Expr *Cond = ignoreCastsAndParens(CO->getCond());
    const Expr *TrueExpr = ignoreCastsAndParens(CO->getTrueExpr());
    if (!Cond || !TrueExpr)
      return true;

    if (!containsPositiveNumlvlsCheck(Cond, TsfbParam))
      return true;

    if (containsSeqSizeGuard(Cond, SeqParam))
      return true;

    const auto *OuterCall = asNamedCall(TrueExpr, "jpc_tsfb_synthesize2");
    if (!OuterCall || OuterCall->getNumArgs() < 2)
      return true;

    const auto *GetRefCall = asNamedCall(OuterCall->getArg(1), "jas_seq2d_getref");
    if (!GetRefCall || GetRefCall->getNumArgs() < 1)
      return true;

    const ValueDecl *ArgDecl = getReferencedDecl(GetRefCall->getArg(0));
    if (!ArgDecl || ArgDecl != SeqParam)
      return true;

    PathDiagnosticLocation Loc(RS->getBeginLoc(), BR.getSourceManager());
    BR.EmitBasicReport(FD, Checker, "Empty sequence reference without size guard",
                       "Custom",
                       "This function takes an internal reference with jas_seq2d_getref(a, ...) and passes it into jpc_tsfb_synthesize2 when only tsfb->numlvls is checked; missing jas_seq2d_size(a) can make the reference invalid for empty sequences.",
                       Loc, RS->getSourceRange());
    Reported = true;
    return true;
  }
};

void EmptySequenceReferenceGuardChecker::checkASTCodeBody(const Decl *D,
                                                          AnalysisManager &,
                                                          BugReporter &BR) const {
  const auto *FD = dyn_cast_or_null<FunctionDecl>(D);
  if (!FD)
    return;
  if (!FD->hasBody())
    return;
  if (FD->getName() != "jpc_tsfb_synthesize")
    return;
  if (FD->param_size() < 2)
    return;

  const ParmVarDecl *TsfbParam = FD->getParamDecl(0);
  const ParmVarDecl *SeqParam = FD->getParamDecl(1);
  if (!TsfbParam || !SeqParam)
    return;

  BodyVisitor Visitor(BR, this, *BT, FD, TsfbParam, SeqParam);
  Visitor.TraverseStmt(FD->getBody());
}

} // namespace

extern "C" void clang_registerCheckers(CheckerRegistry &Registry) {
  Registry.addChecker<EmptySequenceReferenceGuardChecker>(
      "custom.EmptySequenceReferenceGuardChecker",
      "Detects taking a sequence element reference without a non-empty size guard.",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] = CLANG_VERSION_STRING;
