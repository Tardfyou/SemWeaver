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

// Regions whose values are capped by an "input > bound" branch check before a
// power-of-two rounding call; on such a path the rounding cannot overflow.
REGISTER_SET_WITH_PROGRAMSTATE(GuardedInputs, const MemRegion*)

namespace {

// Region of a simple lvalue input: a plain variable or a field such as
// attr->max_entries. Relates a bound-check guard to a producer argument by
// region identity, independent of surrounding implicit casts.
static const MemRegion *getTrackedRegion(const Expr *E, CheckerContext &C) {
  if (!E)
    return nullptr;
  E = E->IgnoreParenImpCasts();

  if (const auto *DRE = dyn_cast<DeclRefExpr>(E)) {
    if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl()))
      return C.getState()->getRegion(VD, C.getLocationContext());
    return nullptr;
  }

  if (const auto *ME = dyn_cast<MemberExpr>(E)) {
    if (const auto *FD = dyn_cast<FieldDecl>(ME->getMemberDecl())) {
      const MemRegion *Base = getTrackedRegion(ME->getBase(), C);
      if (!Base)
        return nullptr;
      return C.getState()->getLValue(FD, loc::MemRegionVal(Base)).getAsRegion();
    }
  }

  return nullptr;
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
      static const CallExpr *findRoundupCall(const Stmt *S, CheckerContext &C);
      static bool isZeroConstant(const Expr *E, CheckerContext &C);
      void recordUpperBoundGuard(const Stmt *Condition, CheckerContext &C) const;
      bool producerInputIsBounded(const Stmt *Origin, CheckerContext &C) const;
      static bool extractCheckedExpr(const Expr *Cond, const Expr* &ECheck, CheckerContext &C);
      void reportIssue(const Stmt *CondSite, const Stmt *OriginCE, CheckerContext &C) const;
};

// Helper: check if a CallExpr calls roundup_pow_of_two(). The kernel macro
// expands to __roundup_pow_of_two() on the non-constant path, so both names
// identify the same power-of-two rounding producer.
bool SAGenTestChecker::isRoundupPow2Call(const CallExpr *CE, CheckerContext &C) {
  if (!CE)
    return false;

  if (const FunctionDecl *FD = CE->getDirectCallee()) {
    if (FD->getIdentifier()) {
      StringRef N = FD->getName();
      if (N == "roundup_pow_of_two" || N == "__roundup_pow_of_two")
        return true;
    }
  }

  // Macro-expanded call sites may not resolve a direct callee: fall back to
  // the spelling of the invocation.
  if (ExprHasName(CE, "roundup_pow_of_two", C))
    return true;
  if (ExprHasName(CE->getCallee(), "roundup_pow_of_two", C))
    return true;

  return false;
}

// Find the rounding producer inside a bind statement. The expanded macro is a
// conditional expression whose branches contain several unrelated calls
// (__builtin_constant_p, ilog2), so looking only at the first child call
// misses the producer.
const CallExpr *SAGenTestChecker::findRoundupCall(const Stmt *S, CheckerContext &C) {
  if (!S)
    return nullptr;
  if (const auto *CE = dyn_cast<CallExpr>(S))
    return isRoundupPow2Call(CE, C) ? CE : nullptr;
  for (const Stmt *Child : S->children()) {
    if (const CallExpr *Found = findRoundupCall(Child, C))
      return Found;
  }
  return nullptr;
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
      ECheck = UO->getSubExpr();
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

  // Find the power-of-two rounding producer within this bind statement (works
  // for init and assignment, including macro-expanded roundup_pow_of_two()).
  const CallExpr *CE = findRoundupCall(S, C);
  if (!CE) {
    // No rounding producer involved -> clear stale info for this variable
    State = State->remove<RoundupResMap>(LReg);
    C.addTransition(State);
    return;
  }

  State = State->set<RoundupResMap>(LReg, CE);
  C.addTransition(State);
}

// The patch fixes the pattern by capping the rounding input with an explicit
// upper-bound branch check ("input > 2^(wordbits-1)") before the call. Record
// such bound comparisons so results produced on the guarded path are trusted.
void SAGenTestChecker::recordUpperBoundGuard(const Stmt *Condition, CheckerContext &C) const {
  const Expr *CondE = dyn_cast_or_null<Expr>(Condition);
  if (!CondE)
    return;

  const auto *BO = dyn_cast<BinaryOperator>(CondE->IgnoreParenImpCasts());
  if (!BO)
    return;

  const Expr *GuardedE = nullptr;
  const Expr *BoundE = nullptr;
  if (BO->getOpcode() == BO_GT) {
    GuardedE = BO->getLHS();
    BoundE = BO->getRHS();
  } else if (BO->getOpcode() == BO_LT) {
    GuardedE = BO->getRHS();
    BoundE = BO->getLHS();
  } else {
    return;
  }

  llvm::APSInt BoundVal;
  if (!EvaluateExprToInt(BoundVal, BoundE, C))
    return;
  if (BoundVal.isNegative() || !BoundVal.isIntN(64))
    return;

  const Expr *GuardedCore = GuardedE->IgnoreParenImpCasts();
  ASTContext &ACtx = C.getASTContext();
  uint64_t InputWidth = ACtx.getTypeSize(GuardedCore->getType());
  if (InputWidth == 0 || InputWidth > 64)
    return;
  // A bound relates to the rounding overflow only when it caps the input at or
  // below 2^(width-1); above that the rounded result no longer fits the word.
  uint64_t BoundU = BoundVal.getZExtValue();
  if (BoundU < (1ULL << (InputWidth - 1)))
    return;
  if (InputWidth < 64 && (BoundU >> InputWidth) != 0)
    return; // bound beyond the input's own range is vacuous

  const MemRegion *MR = getTrackedRegion(GuardedCore, C);
  if (!MR)
    return;
  MR = MR->getBaseRegion();
  if (!MR)
    return;

  ProgramStateRef State = C.getState();
  if (State->contains<GuardedInputs>(MR))
    return;
  C.addTransition(State->add<GuardedInputs>(MR));
}

// True when the producer call's input was capped by a bound-check guard (the
// patch's fix pattern); a zero-test on such a result is then a reliable check.
bool SAGenTestChecker::producerInputIsBounded(const Stmt *Origin, CheckerContext &C) const {
  const auto *CE = dyn_cast_or_null<CallExpr>(Origin);
  if (!CE || CE->getNumArgs() == 0)
    return false;

  const MemRegion *MR = getTrackedRegion(CE->getArg(0), C);
  if (!MR)
    return false;
  MR = MR->getBaseRegion();
  if (!MR)
    return false;

  ProgramStateRef State = C.getState();
  return State->contains<GuardedInputs>(MR);
}

// Detect conditions like !n or n == 0 where n is the result of roundup_pow_of_two(...)
// Also detect !roundup_pow_of_two(x) and roundup_pow_of_two(x) == 0
void SAGenTestChecker::checkBranchCondition(const Stmt *Condition, CheckerContext &C) const {
  const Expr *CondE = dyn_cast_or_null<Expr>(Condition);
  if (!CondE)
    return;

  // Model the fix-side guard: an "input > bound" check caps the value fed to
  // the rounding producer before the call executes.
  recordUpperBoundGuard(Condition, C);

  const Expr *CheckedExpr = nullptr;
  if (!extractCheckedExpr(CondE, CheckedExpr, C))
    return;

  // Case 1: Direct call in the check (e.g., if (!roundup_pow_of_two(x)))
  if (const auto *CE = dyn_cast<CallExpr>(CheckedExpr->IgnoreParenImpCasts())) {
    if (isRoundupPow2Call(CE, C)) {
      if (!producerInputIsBounded(CE, C))
        reportIssue(Condition, CE, C);
      return;
    }
  }

  // Case 2: Variable previously assigned from roundup_pow_of_two(...)
  ProgramStateRef State = C.getState();
  const MemRegion *MR = getTrackedRegion(CheckedExpr, C);
  if (!MR)
    MR = getMemRegionFromExpr(CheckedExpr, C);
  if (!MR)
    return;
  MR = MR->getBaseRegion();
  if (!MR)
    return;

  if (const Stmt *const *OriginPtr = State->get<RoundupResMap>(MR)) {
    const Stmt *Origin = *OriginPtr;
    if (producerInputIsBounded(Origin, C))
      return;
    reportIssue(Condition, Origin, C);
    return;
  }
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detect zero-only validity checks on roundup_pow_of_two() results that lack an input-range guard",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
