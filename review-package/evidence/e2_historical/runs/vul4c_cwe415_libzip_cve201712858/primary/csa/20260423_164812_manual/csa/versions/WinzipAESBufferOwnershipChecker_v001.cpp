#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/Decl.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/Version.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/CheckerManager.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/MemRegion.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramStateTrait.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SVals.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include <memory>

using namespace clang;
using namespace ento;

namespace {

static const Expr *ignoreParenCasts(const Expr *E) {
  return E ? E->IgnoreParenCasts() : nullptr;
}

static const Expr *stripAddrOf(const Expr *E) {
  E = ignoreParenCasts(E);
  if (const auto *UO = dyn_cast_or_null<UnaryOperator>(E)) {
    if (UO->getOpcode() == UO_AddrOf)
      return ignoreParenCasts(UO->getSubExpr());
  }
  return E;
}

static const DeclRefExpr *getDeclRef(const Expr *E) {
  E = stripAddrOf(E);
  return dyn_cast_or_null<DeclRefExpr>(E);
}

static const VarDecl *getVarFromExpr(const Expr *E) {
  const auto *DRE = getDeclRef(E);
  if (!DRE)
    return nullptr;
  return dyn_cast<VarDecl>(DRE->getDecl());
}

static bool exprReferencesVar(const Expr *E, const VarDecl *Target) {
  if (!E || !Target)
    return false;
  E = ignoreParenCasts(E);
  if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
    return DRE->getDecl() == Target;

  for (const Stmt *Child : E->children()) {
    if (const auto *ChildExpr = dyn_cast_or_null<Expr>(Child)) {
      if (exprReferencesVar(ChildExpr, Target))
        return true;
    }
  }
  return false;
}

static bool isIdentifierNamed(const IdentifierInfo *II, llvm::StringRef Name) {
  return II && II->getName() == Name;
}

static bool isCallNamed(const CallExpr *CE, llvm::StringRef Name) {
  if (!CE)
    return false;
  const FunctionDecl *FD = CE->getDirectCallee();
  if (FD && FD->getIdentifier() && FD->getIdentifier()->getName() == Name)
    return true;
  const Expr *Callee = ignoreParenCasts(CE->getCallee());
  if (const auto *DRE = dyn_cast_or_null<DeclRefExpr>(Callee)) {
    if (isIdentifierNamed(DRE->getDecl()->getIdentifier(), Name))
      return true;
  }
  return false;
}

static bool isNegatedExpr(const Expr *Cond, const Expr *&Inner) {
  Cond = ignoreParenCasts(Cond);
  if (const auto *UO = dyn_cast_or_null<UnaryOperator>(Cond)) {
    if (UO->getOpcode() == UO_LNot) {
      Inner = ignoreParenCasts(UO->getSubExpr());
      return true;
    }
  }
  Inner = Cond;
  return false;
}

static bool isNullLikeExpr(const Expr *E) {
  E = ignoreParenCasts(E);
  if (!E)
    return false;
  if (const auto *IL = dyn_cast<IntegerLiteral>(E))
    return IL->getValue().isZero();
  if (isa<CXXNullPtrLiteralExpr>(E))
    return true;
  return false;
}

static bool returnsErrorConstant(const Stmt *S) {
  const auto *RS = dyn_cast_or_null<ReturnStmt>(S);
  if (!RS)
    return false;
  const Expr *Ret = ignoreParenCasts(RS->getRetValue());
  if (!Ret)
    return false;
  if (const auto *IL = dyn_cast<IntegerLiteral>(Ret))
    return !IL->getValue().isZero();
  if (const auto *UO = dyn_cast<UnaryOperator>(Ret)) {
    if (UO->getOpcode() == UO_Minus) {
      const Expr *Sub = ignoreParenCasts(UO->getSubExpr());
      if (const auto *IL = dyn_cast_or_null<IntegerLiteral>(Sub))
        return !IL->getValue().isZero();
    }
  }
  return false;
}

static bool stmtContainsFreeOfVar(const Stmt *S, const VarDecl *Var) {
  if (!S || !Var)
    return false;
  if (const auto *CE = dyn_cast<CallExpr>(S)) {
    if (isCallNamed(CE, "_zip_buffer_free") && CE->getNumArgs() >= 1) {
      if (getVarFromExpr(CE->getArg(0)) == Var)
        return true;
    }
  }
  for (const Stmt *Child : S->children()) {
    if (stmtContainsFreeOfVar(Child, Var))
      return true;
  }
  return false;
}

static bool stmtContainsReturnError(const Stmt *S) {
  if (!S)
    return false;
  if (returnsErrorConstant(S))
    return true;
  for (const Stmt *Child : S->children()) {
    if (stmtContainsReturnError(Child))
      return true;
  }
  return false;
}

static bool stmtHasOwnershipGuard(const Stmt *S, const VarDecl *OwnershipFlag) {
  if (!S || !OwnershipFlag)
    return false;
  if (const auto *IfS = dyn_cast<IfStmt>(S)) {
    const Expr *Cond = IfS->getCond();
    const Expr *Inner = nullptr;
    const bool Negated = isNegatedExpr(Cond, Inner);
    if (Negated && exprReferencesVar(Inner, OwnershipFlag))
      return true;
  }
  for (const Stmt *Child : S->children()) {
    if (stmtHasOwnershipGuard(Child, OwnershipFlag))
      return true;
  }
  return false;
}

static bool isFailureCheckForCall(const Expr *Cond, const CallExpr *TargetCall) {
  if (!Cond || !TargetCall)
    return false;
  const Expr *Inner = nullptr;
  if (isNegatedExpr(Cond, Inner)) {
    return Inner == TargetCall;
  }

  Cond = ignoreParenCasts(Cond);
  if (const auto *BO = dyn_cast<BinaryOperator>(Cond)) {
    if (BO->isComparisonOp()) {
      const Expr *LHS = ignoreParenCasts(BO->getLHS());
      const Expr *RHS = ignoreParenCasts(BO->getRHS());
      if ((LHS == TargetCall && isNullLikeExpr(RHS)) ||
          (RHS == TargetCall && isNullLikeExpr(LHS)))
        return true;
    }
  }
  return false;
}

class WinzipAESBufferOwnershipChecker : public Checker<check::ASTCodeBody> {
  mutable std::unique_ptr<BugType> BT;

public:
  WinzipAESBufferOwnershipChecker()
      : BT(std::make_unique<BugType>(this,
                                     "Borrowed buffer freed on callee failure",
                                     "Memory error")) {}

  void checkASTCodeBody(const Decl *D, AnalysisManager &, BugReporter &BR) const {
    const auto *FD = dyn_cast_or_null<FunctionDecl>(D);
    if (!FD || !FD->hasBody())
      return;

    const Stmt *Body = FD->getBody();
    if (!Body)
      return;

    const VarDecl *BorrowFlag = nullptr;
    findBorrowFlag(Body, BorrowFlag);
    if (!BorrowFlag)
      return;

    inspectStmtTree(Body, BorrowFlag, BR);
  }

private:
  static void findBorrowFlag(const Stmt *S, const VarDecl *&Result) {
    if (!S || Result)
      return;

    if (const auto *IfS = dyn_cast<IfStmt>(S)) {
      const Expr *Cond = IfS->getCond();
      const Expr *Inner = nullptr;
      if (isNegatedExpr(Cond, Inner)) {
        if (const VarDecl *VD = getVarFromExpr(Inner)) {
          Result = VD;
          return;
        }
      }
    }

    for (const Stmt *Child : S->children())
      findBorrowFlag(Child, Result);
  }

  void inspectStmtTree(const Stmt *S, const VarDecl *BorrowFlag,
                       BugReporter &BR) const {
    if (!S || !BorrowFlag)
      return;

    if (const auto *IfS = dyn_cast<IfStmt>(S))
      inspectIfStmt(IfS, BorrowFlag, BR);

    for (const Stmt *Child : S->children())
      inspectStmtTree(Child, BorrowFlag, BR);
  }

  void inspectIfStmt(const IfStmt *IfS, const VarDecl *BorrowFlag,
                     BugReporter &BR) const {
    if (!IfS || !BorrowFlag)
      return;

    const Expr *Cond = ignoreParenCasts(IfS->getCond());
    if (!Cond)
      return;

    const auto *Call = dyn_cast<CallExpr>(Cond);
    if (!Call) {
      const Expr *Inner = nullptr;
      if (isNegatedExpr(Cond, Inner))
        Call = dyn_cast_or_null<CallExpr>(Inner);
      else if (const auto *BO = dyn_cast<BinaryOperator>(Cond)) {
        const Expr *LHS = ignoreParenCasts(BO->getLHS());
        const Expr *RHS = ignoreParenCasts(BO->getRHS());
        if (const auto *LCall = dyn_cast<CallExpr>(LHS))
          Call = LCall;
        else if (const auto *RCall = dyn_cast<CallExpr>(RHS))
          Call = RCall;
      }
    }
    if (!Call)
      return;

    if (!isFailureCheckForCall(Cond, Call))
      return;

    if (!isWinzipAESLikeProcessingCall(Call))
      return;

    const Stmt *Then = IfS->getThen();
    if (!Then)
      return;

    const VarDecl *FreedVar = findFreedBorrowedVar(Then, BorrowFlag);
    if (!FreedVar)
      return;

    if (!stmtContainsReturnError(Then))
      return;

    if (!calleeReceivesResource(Call, FreedVar))
      return;

    PathDiagnosticLocation Loc = PathDiagnosticLocation::createBegin(
        Then->getBeginLoc(), BR.getSourceManager());
    SmallVector<SourceRange, 2> Ranges;
    if (const Expr *ArgExpr = findCallArgReferencingVar(Call, FreedVar))
      Ranges.push_back(ArgExpr->getSourceRange());
    Ranges.push_back(Then->getSourceRange());

    BR.EmitBasicReport(FreedVar, this,
                       "Borrowed buffer freed on ownership-sensitive failure",
                       "Memory error",
                       "A buffer marked as externally owned is released in an "
                       "error path immediately after a callee that received the "
                       "same resource fails; this suggests mismatched ownership "
                       "or double-free risk if the callee may consume or clean up "
                       "the buffer during failure handling.",
                       Loc, Ranges);
  }

  static bool isWinzipAESLikeProcessingCall(const CallExpr *CE) {
    if (!CE)
      return false;
    const FunctionDecl *FD = CE->getDirectCallee();
    if (!FD || !FD->getIdentifier())
      return false;
    llvm::StringRef Name = FD->getIdentifier()->getName();
    return Name.contains("process") && Name.contains("aes");
  }

  static const VarDecl *findFreedBorrowedVar(const Stmt *S,
                                             const VarDecl *BorrowFlag) {
    if (!S)
      return nullptr;

    if (const auto *IfS = dyn_cast<IfStmt>(S)) {
      if (stmtHasOwnershipGuard(IfS->getThen(), BorrowFlag)) {
        if (const VarDecl *Freed = findFreedBorrowedVar(IfS->getThen(), nullptr))
          return Freed;
      }
      if (stmtHasOwnershipGuard(IfS->getElse(), BorrowFlag)) {
        if (const VarDecl *Freed = findFreedBorrowedVar(IfS->getElse(), nullptr))
          return Freed;
      }
    }

    if (const auto *CE = dyn_cast<CallExpr>(S)) {
      if (isCallNamed(CE, "_zip_buffer_free") && CE->getNumArgs() >= 1)
        return getVarFromExpr(CE->getArg(0));
    }

    for (const Stmt *Child : S->children()) {
      if (const VarDecl *Freed = findFreedBorrowedVar(Child, BorrowFlag)) {
        if (!BorrowFlag)
          return Freed;
        return Freed;
      }
    }
    return nullptr;
  }

  static bool calleeReceivesResource(const CallExpr *CE, const VarDecl *Var) {
    return findCallArgReferencingVar(CE, Var) != nullptr;
  }

  static const Expr *findCallArgReferencingVar(const CallExpr *CE,
                                               const VarDecl *Var) {
    if (!CE || !Var)
      return nullptr;
    for (const Expr *Arg : CE->arguments()) {
      if (exprReferencesVar(Arg, Var))
        return Arg;
    }
    return nullptr;
  }
};

} // namespace

extern "C" void clang_registerCheckers(CheckerRegistry &Registry) {
  Registry.addChecker<WinzipAESBufferOwnershipChecker>(
      "custom.WinzipAESBufferOwnershipChecker",
      "Detects freeing of externally owned buffers on failure paths after ownership-sensitive processing calls.",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] = CLANG_VERSION_STRING;
