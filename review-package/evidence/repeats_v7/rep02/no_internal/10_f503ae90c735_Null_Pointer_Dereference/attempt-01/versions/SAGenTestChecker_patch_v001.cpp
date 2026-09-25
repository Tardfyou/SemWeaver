// /external/upstream-study/SAGEN/result-0224-bugfail-multi-o3mini/test-Null-Pointer-Dereference-f503ae90c7355e8506e68498fe84c1357894cd5b/checkers/checker0.cpp
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
#include "clang/AST/Expr.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"

using namespace clang;
using namespace ento;
using namespace taint;

// Register program state maps.
// This map records pointers (identified by their base MemRegion) returned by
// mt76_connac_get_he_phy_cap and whether they have been null-checked.
REGISTER_MAP_WITH_PROGRAMSTATE(PossibleNullPtrMap, const MemRegion*, bool)
// Optional pointer alias map to track aliasing between pointer regions.
REGISTER_MAP_WITH_PROGRAMSTATE(PtrAliasMap, const MemRegion*, const MemRegion*)
// Maps a branch condition to the pointer it proves non-NULL on one successor.
REGISTER_MAP_WITH_PROGRAMSTATE(GuardedConditionMap, SymbolRef, const MemRegion*)
REGISTER_MAP_WITH_PROGRAMSTATE(NonNullAssumptionMap, SymbolRef, bool)

namespace {

/// setChecked - Helper function that marks a pointer's MemRegion (and its alias,
/// if any) in the PossibleNullPtrMap as "checked" (i.e. true).
ProgramStateRef setChecked(ProgramStateRef State, const MemRegion *MR) {
  if (!MR)
    return State;
  // If the pointer has not been marked as checked, mark it.
  const bool *Checked = State->get<PossibleNullPtrMap>(MR);
  if (Checked && *Checked == false) {
    State = State->set<PossibleNullPtrMap>(MR, true);
  }
  // Update the alias (if one exists) so that both sides are marked as checked.
  if (const auto *AliasLookup = State->get<PtrAliasMap>(MR)) {
    const MemRegion *Alias = *AliasLookup;
    const bool *AliasChecked = State->get<PossibleNullPtrMap>(Alias);
    if (AliasChecked && *AliasChecked == false) {
      State = State->set<PossibleNullPtrMap>(Alias, true);
    }
  }
  return State;
}

/// The checker class.
class SAGenTestChecker
  : public Checker< check::PostCall,
                    check::BranchCondition,
                    check::Location,
                    check::Bind,
                    eval::Assume > {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker() 
    : BT(new BugType(this, "Missing NULL check for mt76_connac_get_he_phy_cap return value")) {}

  // Callback for recording the return value.
  void checkPostCall(const CallEvent &Call, CheckerContext &C) const;

  // Callback for recording which branch proves a pointer non-NULL.
  void checkBranchCondition(const Stmt *Condition, CheckerContext &C) const;

  // Callback for applying branch-specific NULL-check state.
  ProgramStateRef evalAssume(ProgramStateRef State, SVal Cond,
                             bool Assumption) const;

  // Callback for detecting dereferences (i.e. loads) of unchecked pointers.
  void checkLocation(SVal Loc, bool isLoad, const Stmt *S, CheckerContext &C) const;

  // Callback for tracking pointer aliasing.
  void checkBind(SVal Loc, SVal Val, const Stmt *StoreE, CheckerContext &C) const;
};

/// checkPostCall
/// This callback detects calls to mt76_connac_get_he_phy_cap. It retrieves the
/// returned pointer (its base region) and marks it in the PossibleNullPtrMap as not null-checked.
void SAGenTestChecker::checkPostCall(const CallEvent &Call, CheckerContext &C) const {
  ProgramStateRef State = C.getState();
  const Expr *Origin = Call.getOriginExpr();
  if (!Origin)
    return;
  // Identify the target function call by name using the utility function.
  if (!ExprHasName(Origin, "mt76_connac_get_he_phy_cap", C))
    return;
  
  // Get the memory region corresponding to the call's return value.
  const MemRegion *MR = getMemRegionFromExpr(Origin, C);
  if (!MR)
    return;
  MR = MR->getBaseRegion();

  // Record that this pointer has not been checked for NULL.
  State = State->set<PossibleNullPtrMap>(MR, false);
  C.addTransition(State);
}

/// checkBranchCondition
/// Record the condition successor on which the tested pointer is known non-NULL.
void SAGenTestChecker::checkBranchCondition(const Stmt *Condition, CheckerContext &C) const {
  ProgramStateRef State = C.getState();
  const Expr *CondE = dyn_cast<Expr>(Condition);
  if (!CondE)
    return;
  CondE = CondE->IgnoreParenCasts();

  const Expr *PtrExpr = nullptr;
  bool NonNullAssumption = true;
  if (const auto *UO = dyn_cast<UnaryOperator>(CondE)) {
    if (UO->getOpcode() == UO_LNot) {
      PtrExpr = UO->getSubExpr()->IgnoreParenCasts();
      NonNullAssumption = false;
    }
  } else if (const auto *BO = dyn_cast<BinaryOperator>(CondE)) {
    BinaryOperator::Opcode Op = BO->getOpcode();
    if (Op == BO_EQ || Op == BO_NE) {
      const Expr *LHS = BO->getLHS()->IgnoreParenCasts();
      const Expr *RHS = BO->getRHS()->IgnoreParenCasts();
      bool LHSIsNull = LHS->isNullPointerConstant(
          C.getASTContext(), Expr::NPC_ValueDependentIsNull);
      bool RHSIsNull = RHS->isNullPointerConstant(
          C.getASTContext(), Expr::NPC_ValueDependentIsNull);
      if (LHSIsNull != RHSIsNull) {
        PtrExpr = LHSIsNull ? RHS : LHS;
        NonNullAssumption = Op == BO_NE;
      }
    }
  } else {
    PtrExpr = CondE;
  }

  if (!PtrExpr)
    return;
  SVal PtrVal = State->getSVal(PtrExpr, C.getLocationContext());
  const MemRegion *MR = PtrVal.getAsRegion();
  SymbolRef CondSym = State->getSVal(CondE, C.getLocationContext()).getAsSymbol();
  if (!MR || !CondSym)
    return;
  State = State->set<GuardedConditionMap>(CondSym, MR->getBaseRegion());
  State = State->set<NonNullAssumptionMap>(CondSym, NonNullAssumption);
  C.addTransition(State);
}

ProgramStateRef SAGenTestChecker::evalAssume(ProgramStateRef State, SVal Cond,
                                             bool Assumption) const {
  SymbolRef CondSym = Cond.getAsSymbol();
  if (!CondSym)
    return State;
  const MemRegion *const *MR = State->get<GuardedConditionMap>(CondSym);
  const bool *NonNullAssumption = State->get<NonNullAssumptionMap>(CondSym);
  if (!MR || !NonNullAssumption || Assumption != *NonNullAssumption)
    return State;
  return setChecked(State, *MR);
}

/// checkLocation
/// This callback is triggered on a load (i.e. a dereference). If the memory location
/// being loaded is derived from a pointer that was never null-checked (i.e. marked false
/// in PossibleNullPtrMap), emit a bug report.
void SAGenTestChecker::checkLocation(SVal Loc, bool isLoad, const Stmt *S, CheckerContext &C) const {
  // We are interested only in load operations.
  if (!isLoad)
    return;
  ProgramStateRef State = C.getState();
  const MemRegion *MR = Loc.getAsRegion();
  if (!MR)
    return;
  MR = MR->getBaseRegion();
  const bool *Checked = State->get<PossibleNullPtrMap>(MR);
  if (Checked && *Checked == false) {
    // The pointer is being dereferenced without a NULL check.
    ExplodedNode *N = C.generateNonFatalErrorNode();
    if (!N)
      return;
    auto Report = std::make_unique<PathSensitiveBugReport>(
        *BT,
        "Missing NULL check for mt76_connac_get_he_phy_cap return value", N);
    C.emitReport(std::move(Report));
  }
}

/// checkBind
/// This callback tracks pointer aliasing. When one pointer is assigned to another,
/// record their relationship so that if one is marked as checked, its alias is updated.
void SAGenTestChecker::checkBind(SVal Loc, SVal Val, const Stmt *StoreE, CheckerContext &C) const {
  ProgramStateRef State = C.getState();
  const MemRegion *LHS = Loc.getAsRegion();
  if (!LHS)
    return;
  LHS = LHS->getBaseRegion();
  const MemRegion *RHS = Val.getAsRegion();
  if (!RHS)
    return;
  RHS = RHS->getBaseRegion();
  State = State->set<PtrAliasMap>(LHS, RHS);
  State = State->set<PtrAliasMap>(RHS, LHS);
  C.addTransition(State);
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker", 
      "Detects missing NULL check for the return value of mt76_connac_get_he_phy_cap", 
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
