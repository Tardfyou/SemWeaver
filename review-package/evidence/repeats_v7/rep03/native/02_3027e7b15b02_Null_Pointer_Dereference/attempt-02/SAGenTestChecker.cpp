// /external/upstream-study/SAGEN/result-0224-bugfail-multi-o3mini/test-Null-Pointer-Dereference-3027e7b15b02d2d37e3f82d6b8404f6d37e3b8cf/checkers/checker2.cpp
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

using namespace clang;
using namespace ento;
using namespace taint;

//------------------------------------------------------------------------------
// Program state maps:
//   PossibleNullPtrMap: Record devm_kasprintf return regions and whether they
//                       have been checked for NULL. (false means unchecked,
//                       true means checked)
//   PtrAliasMap: Tracks aliasing between pointer regions.
//------------------------------------------------------------------------------
REGISTER_MAP_WITH_PROGRAMSTATE(PossibleNullPtrMap, const MemRegion*, bool)
REGISTER_MAP_WITH_PROGRAMSTATE(PtrAliasMap, const MemRegion*, const MemRegion*)
REGISTER_MAP_WITH_PROGRAMSTATE(NullCheckRegionMap, SymbolRef, const MemRegion*)
REGISTER_MAP_WITH_PROGRAMSTATE(NullCheckNonNullMap, SymbolRef, bool)

//------------------------------------------------------------------------------
// Helper function: setChecked
//
// Mark a given memory region as "checked" (i.e. its value has been tested against NULL).
// Also propagate the check to any aliased region recorded in PtrAliasMap.
//------------------------------------------------------------------------------
static ProgramStateRef setChecked(ProgramStateRef State, const MemRegion *MR) {
  if (!MR)
    return State;

  const bool *Tracked = State->get<PossibleNullPtrMap>(MR);
  const auto *Alias = State->get<PtrAliasMap>(MR);
  const bool *TrackedAlias =
      Alias ? State->get<PossibleNullPtrMap>(*Alias) : nullptr;
  if (!Tracked && !TrackedAlias)
    return State;

  State = State->set<PossibleNullPtrMap>(MR, true);
  if (Alias && TrackedAlias)
    State = State->set<PossibleNullPtrMap>(*Alias, true);
  return State;
}

namespace {

class SAGenTestChecker : public Checker<
    check::PostCall,        // For intercepting devm_kasprintf return values.
    check::PreCall,         // For catching unchecked pointer consumption.
    check::BranchCondition, // For detecting null-checks.
    eval::Assume,           // For applying checks only on the non-NULL path.
    check::Bind             // For propagating the pointer's null-check state.
    > {
  mutable std::unique_ptr<BugType> BT;
  
public:
  SAGenTestChecker() : BT(new BugType(this, "Unchecked devm_kasprintf return", "Null Dereference")) {}

  // Callback: After a function call is evaluated.
  void checkPostCall(const CallEvent &Call, CheckerContext &C) const;

  // Callback: Apply a pending NULL check to its feasible non-NULL successor.
  ProgramStateRef evalAssume(ProgramStateRef State, SVal Cond,
                             bool Assumption) const;

  // Callback: When an assignment (binding) occurs.
  void checkBind(SVal Loc, SVal Val, const Stmt *StoreE, CheckerContext &C) const;

  // Callback: When a branch condition is evaluated.
  void checkBranchCondition(const Stmt *Condition, CheckerContext &C) const;

  // Callback: Before a call consumes an argument.
  void checkPreCall(const CallEvent &Call, CheckerContext &C) const;

private:
  // Helper for reporting the bug.
  void reportUncheckedUse(const Stmt *S, CheckerContext &C) const;
};

/// checkPostCall - Intercept calls to devm_kasprintf. If the function is called,
/// mark its returned memory region as "unchecked" (false) in PossibleNullPtrMap.
void SAGenTestChecker::checkPostCall(const CallEvent &Call, CheckerContext &C) const {
  const Expr *OriginExpr = Call.getOriginExpr();
  if (!OriginExpr)
    return;
  
  // Use the utility function to check if this call is to devm_kasprintf.
  if (!ExprHasName(OriginExpr, "devm_kasprintf", C))
    return;
  
  // Retrieve the memory region for the returned pointer.
  const MemRegion *MR = getMemRegionFromExpr(OriginExpr, C);
  if (!MR)
    return;
  MR = MR->getBaseRegion();
  if (!MR)
    return;
  
  ProgramStateRef State = C.getState();
  State = State->set<PossibleNullPtrMap>(MR, false);
  C.addTransition(State);
}

/// checkBind - Propagate the null-check state of devm_kasprintf pointers.
/// When a value is bound to a new memory region (as in pointer assignment),
/// copy the unchecked status from the RHS to the LHS.
void SAGenTestChecker::checkBind(SVal Loc, SVal Val, const Stmt *StoreE, CheckerContext &C) const {
  ProgramStateRef State = C.getState();
  const MemRegion *LHSReg = Loc.getAsRegion();
  if (!LHSReg)
    return;
  LHSReg = LHSReg->getBaseRegion();
  if (!LHSReg)
    return;
  
  if (const MemRegion *RHSReg = Val.getAsRegion()) {
    RHSReg = RHSReg->getBaseRegion();
    if (!RHSReg)
      return;
    
    // If the RHS pointer is tracked in PossibleNullPtrMap, propagate its check status.
    if (State->get<PossibleNullPtrMap>(RHSReg)) {
      bool Checked = *State->get<PossibleNullPtrMap>(RHSReg);
      State = State->set<PossibleNullPtrMap>(LHSReg, Checked);
    }
    // Update pointer alias map.
    State = State->set<PtrAliasMap>(LHSReg, RHSReg);
    State = State->set<PtrAliasMap>(RHSReg, LHSReg);
    C.addTransition(State);
  }
}

/// checkBranchCondition - Inspect branch conditions for NULL-checks.
/// If a condition is detected that compares a pointer (from devm_kasprintf)
/// against NULL, mark that pointer as checked.
void SAGenTestChecker::checkBranchCondition(const Stmt *Condition,
                                              CheckerContext &C) const {
  ProgramStateRef State = C.getState();
  const Expr *CondE = dyn_cast<Expr>(Condition);
  if (!CondE) {
    C.addTransition(State);
    return;
  }

  CondE = CondE->IgnoreParenImpCasts();
  const Expr *PtrExpr = nullptr;
  bool NonNullOnTrue = true;

  if (const auto *UO = dyn_cast<UnaryOperator>(CondE)) {
    if (UO->getOpcode() == UO_LNot) {
      PtrExpr = UO->getSubExpr()->IgnoreParenImpCasts();
      NonNullOnTrue = false;
    }
  } else if (const auto *BO = dyn_cast<BinaryOperator>(CondE)) {
    if (BO->getOpcode() == BO_EQ || BO->getOpcode() == BO_NE) {
      const Expr *LHS = BO->getLHS()->IgnoreParenImpCasts();
      const Expr *RHS = BO->getRHS()->IgnoreParenImpCasts();
      bool LHSIsNull = LHS->isNullPointerConstant(
          C.getASTContext(), Expr::NPC_ValueDependentIsNull);
      bool RHSIsNull = RHS->isNullPointerConstant(
          C.getASTContext(), Expr::NPC_ValueDependentIsNull);
      if (LHSIsNull != RHSIsNull) {
        PtrExpr = LHSIsNull ? RHS : LHS;
        NonNullOnTrue = BO->getOpcode() == BO_NE;
      }
    }
  } else {
    PtrExpr = CondE;
  }

  if (PtrExpr) {
    SVal PtrVal = State->getSVal(PtrExpr, C.getLocationContext());
    if (const MemRegion *MR = PtrVal.getAsRegion()) {
      MR = MR->getBaseRegion();
      const MemRegion *TrackedMR = MR;
      if (MR && !State->get<PossibleNullPtrMap>(MR)) {
        if (const auto *Alias = State->get<PtrAliasMap>(MR))
          TrackedMR = *Alias;
      }
      if (TrackedMR && State->get<PossibleNullPtrMap>(TrackedMR)) {
        SVal CondVal = State->getSVal(CondE, C.getLocationContext());
        if (SymbolRef CondSymbol = CondVal.getAsSymbol()) {
          State = State->set<NullCheckRegionMap>(CondSymbol, MR);
          State = State->set<NullCheckNonNullMap>(CondSymbol,
                                                   NonNullOnTrue);
        }
      }
    }
  }

  C.addTransition(State);
}

ProgramStateRef SAGenTestChecker::evalAssume(ProgramStateRef State, SVal Cond,
                                              bool Assumption) const {
  SymbolRef CondSymbol = Cond.getAsSymbol();
  if (!CondSymbol)
    return State;

  const auto *MR = State->get<NullCheckRegionMap>(CondSymbol);
  const bool *NonNullOnTrue = State->get<NullCheckNonNullMap>(CondSymbol);
  if (!MR || !NonNullOnTrue)
    return State;

  State = State->remove<NullCheckRegionMap>(CondSymbol);
  State = State->remove<NullCheckNonNullMap>(CondSymbol);
  if (Assumption == *NonNullOnTrue)
    State = setChecked(State, *MR);
  return State;
}

/// checkPreCall - Report when an unchecked devm_kasprintf result is passed to
/// a consumer, rather than when its storage is merely loaded.
void SAGenTestChecker::checkPreCall(const CallEvent &Call,
                                    CheckerContext &C) const {
  ProgramStateRef State = C.getState();
  for (unsigned I = 0; I < Call.getNumArgs(); ++I) {
    const MemRegion *MR = Call.getArgSVal(I).getAsRegion();
    if (!MR)
      continue;

    MR = MR->getBaseRegion();
    if (!MR)
      continue;

    const bool *Checked = State->get<PossibleNullPtrMap>(MR);
    if (!Checked) {
      if (const auto *Alias = State->get<PtrAliasMap>(MR))
        Checked = State->get<PossibleNullPtrMap>(*Alias);
    }
    if (Checked && !*Checked)
      reportUncheckedUse(Call.getArgExpr(I), C);
  }
}

/// reportUncheckedUse - Generate a bug report when an unchecked devm_kasprintf
/// pointer is dereferenced.
void SAGenTestChecker::reportUncheckedUse(const Stmt *S, CheckerContext &C) const {
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;
  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT, "Unchecked devm_kasprintf return value used", N);
  Report->addRange(S->getSourceRange());
  C.emitReport(std::move(Report));
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker", 
      "Detects use of devm_kasprintf return value without NULL checking",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
