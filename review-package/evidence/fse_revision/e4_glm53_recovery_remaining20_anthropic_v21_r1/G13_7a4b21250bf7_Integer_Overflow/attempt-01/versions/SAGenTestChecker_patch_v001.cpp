#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Checkers/Taint.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/Environment.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramState.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramStateTrait.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SymExpr.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtVisitor.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/SmallVector.h"
#include "clang/StaticAnalyzer/Checkers/utility.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"

using namespace clang;
using namespace ento;
using namespace taint;

// Map variables that currently hold the result of roundup_pow_of_two(...)
// We keep the originating Stmt* (CallExpr) just for diagnostic purposes.
REGISTER_MAP_WITH_PROGRAMSTATE(RoundupResMap, const MemRegion*, const Stmt*)

namespace {

// Collect every CallExpr in a statement tree. Needed because
// roundup_pow_of_two() is a macro expanding to
// __builtin_constant_p(n) ? <const path> : __roundup_pow_of_two(n),
// so a bind can contain several nested calls and the tracked producer
// is not necessarily the first one visited.
static void collectCallExprs(const Stmt *S,
                             llvm::SmallVectorImpl<const CallExpr *> &Out) {
  if (!S)
    return;
  if (const auto *CE = dyn_cast<CallExpr>(S))
    Out.push_back(CE);
  for (const Stmt *Child : S->children())
    collectCallExprs(Child, Out);
}

class SAGenTestChecker
  : public Checker<
        check::Bind,
        check::BranchCondition
    > {
   mutable std::unique_ptr<BugType> BT;

   public:
      SAGenTestChecker()
          : BT(new BugType(this, "Unreliable overflow check after roundup_pow_of_two()",
                                 "Logic error")) {}

      void checkBind(SVal Loc, SVal Val, const Stmt *S, CheckerContext &C) const;
      void checkBranchCondition(const Stmt *Condition, CheckerContext &C) const;

   private:
      // Helpers
      static bool isRoundupPow2Call(const CallExpr *CE, CheckerContext &C);
      static bool isZeroConstant(const Expr *E, CheckerContext &C);
      static bool extractCheckedExpr(const Expr *Cond, const Expr* &ECheck, CheckerContext &C);
      void reportIssue(const Stmt *CondSite, const Stmt *OriginCE, CheckerContext &C) const;
};

// Helper: check if a CallExpr calls roundup_pow_of_two(...) or the
// out-of-line helper __roundup_pow_of_two(...) produced by its macro
// expansion; the analyzer usually only sees the helper call.
bool SAGenTestChecker::isRoundupPow2Call(const CallExpr *CE, CheckerContext &C) {
  if (!CE)
    return false;

  // Exact callee-identifier match: the rounding API itself supplies the
  // overflow semantics being tracked.
  if (const FunctionDecl *FD = CE->getDirectCallee()) {
    if (FD->getIdentifier()) {
      llvm::StringRef Name = FD->getName();
      if (Name == "roundup_pow_of_two" || Name == "__roundup_pow_of_two")
        return true;
    }
  }

  // Fall back to source-text name checks (covers direct macro spellings)
  if (ExprHasName(CE, "roundup_pow_of_two", C))
    return true;
  if (ExprHasName(CE->getCallee(), "roundup_pow_of_two", C))
    return true;

  return false;
}

// Helper: determine if expression equals integer zero (or null constant)
bool SAGenTestChecker::isZeroConstant(const Expr *E, CheckerContext &C) {
  if (!E)
    return false;

  llvm::APSInt Res;
  if (EvaluateExprToInt(Res, E, C))
    return Res == 0;

  if (const auto *IL = dyn_cast<IntegerLiteral>(E->IgnoreParenCasts()))
    return IL->getValue() == 0;

  // Also consider NULL-like constants
  if (E->isNullPointerConstant(C.getASTContext(), Expr::NPC_ValueDependentIsNull))
    return true;

  return false;
}

// Helper: from a condition, extract the expression that is being checked for zero
// Match: !X  -> ECheck = X
//        X == 0 or 0 == X -> ECheck = X
bool SAGenTestChecker::extractCheckedExpr(const Expr *Cond, const Expr* &ECheck, CheckerContext &C) {
  if (!Cond)
    return false;

  const Expr *ECond = Cond->IgnoreParenImpCasts();

  if (const auto *UO = dyn_cast<UnaryOperator>(ECond)) {
    if (UO->getOpcode() == UO_LNot) {
      // Strip casts/promotions so the underlying lvalue region is found
      // (n_buckets is u64 while the producer returns unsigned long).
      ECheck = UO->getSubExpr()->IgnoreParenImpCasts();
      return true;
    }
  }

  if (const auto *BO = dyn_cast<BinaryOperator>(ECond)) {
    if (BO->getOpcode() == BO_EQ) {
      const Expr *L = BO->getLHS();
      const Expr *R = BO->getRHS();
      if (isZeroConstant(L, C)) {
        ECheck = R;
        return true;
      }
      if (isZeroConstant(R, C)) {
        ECheck = L;
        return true;
      }
    }
  }

  return false;
}

// Report a concise warning
void SAGenTestChecker::reportIssue(const Stmt *CondSite, const Stmt *OriginCE, CheckerContext &C) const {
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto R = std::make_unique<PathSensitiveBugReport>(
      *BT,
      "Unreliable overflow check after roundup_pow_of_two()",
      N);

  if (CondSite)
    R->addRange(CondSite->getSourceRange());

  if (OriginCE) {
    PathDiagnosticLocation Loc =
        PathDiagnosticLocation::createBegin(OriginCE, C.getSourceManager(), C.getLocationContext());
    R->addNote("Value comes from roundup_pow_of_two() here", Loc);
  }

  if (CondSite) {
    PathDiagnosticLocation Loc2 =
        PathDiagnosticLocation::createBegin(CondSite, C.getSourceManager(), C.getLocationContext());
    R->addNote("roundup_pow_of_two() may overflow (UB) on 32-bit; check the input before rounding (e.g., x > 1UL << 31).", Loc2);
  }

  C.emitReport(std::move(R));
}

// Track binds: mark variables assigned from roundup_pow_of_two(...); clear otherwise
void SAGenTestChecker::checkBind(SVal Loc, SVal Val, const Stmt *S, CheckerContext &C) const {
  const MemRegion *LReg = Loc.getAsRegion();
  if (!LReg)
    return;
  LReg = LReg->getBaseRegion();
  if (!LReg)
    return;

  ProgramStateRef State = C.getState();

  // Scan every CallExpr in this bind stmt (works for init and assignment).
  // The roundup_pow_of_two() macro yields several nested calls, so the
  // tracked producer is any roundup call in the bound expression.
  llvm::SmallVector<const CallExpr *, 4> Calls;
  collectCallExprs(S, Calls);

  const CallExpr *RoundupCE = nullptr;
  for (const CallExpr *CE : Calls) {
    if (isRoundupPow2Call(CE, C)) {
      RoundupCE = CE;
      break;
    }
  }

  // Track only regions whose value is produced by roundup_pow_of_two(...)
  if (RoundupCE) {
    State = State->set<RoundupResMap>(LReg, RoundupCE);
  } else {
    State = State->remove<RoundupResMap>(LReg);
  }

  C.addTransition(State);
}

// Detect conditions like !n or n == 0 where n is the result of roundup_pow_of_two(...)
// Also detect !roundup_pow_of_two(x) and roundup_pow_of_two(x) == 0
void SAGenTestChecker::checkBranchCondition(const Stmt *Condition, CheckerContext &C) const {
  // No target-width gate: the zero-test sentinel is unreliable for
  // roundup_pow_of_two() on any width, because its overflow is undefined
  // behavior (shift count reaching the result type width) instead of
  // wrapping to zero. The patch replaces the sentinel with an input bound
  // (max_entries > 1UL << 31) evaluated before the call, which removes the
  // zero-test the sink below requires.
  const Expr *CondE = dyn_cast_or_null<Expr>(Condition);
  if (!CondE)
    return;

  const Expr *CheckedExpr = nullptr;
  if (!extractCheckedExpr(CondE, CheckedExpr, C))
    return;

  // Case 1: Direct call in the check (e.g., if (!roundup_pow_of_two(x)))
  if (const auto *CE = dyn_cast<CallExpr>(CheckedExpr->IgnoreParenImpCasts())) {
    if (isRoundupPow2Call(CE, C)) {
      reportIssue(Condition, CE, C);
      return;
    }
  }

  // Case 2: Variable previously assigned from roundup_pow_of_two(...)
  ProgramStateRef State = C.getState();
  const MemRegion *MR = getMemRegionFromExpr(CheckedExpr, C);
  if (!MR)
    return;
  MR = MR->getBaseRegion();
  if (!MR)
    return;

  if (const Stmt *const *OriginPtr = State->get<RoundupResMap>(MR)) {
    const Stmt *Origin = *OriginPtr;
    reportIssue(Condition, Origin, C);
    return;
  }
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detect unreliable overflow checks after roundup_pow_of_two() on 32-bit targets",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
