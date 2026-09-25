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
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtVisitor.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/StaticAnalyzer/Checkers/utility.h"

// Additional necessary includes
#include "clang/Lex/Lexer.h"
#include "clang/Basic/LangOptions.h"

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
REGISTER_MAP_WITH_PROGRAMSTATE(PendingNullCheck, const MemRegion*, bool)

//------------------------------------------------------------------------------
// Helper function: setChecked
//
// Mark a given memory region as "checked" (i.e. its value has been tested against NULL).
// Also propagate the check to any aliased region recorded in PtrAliasMap.
//------------------------------------------------------------------------------
static ProgramStateRef setChecked(ProgramStateRef State, const MemRegion *MR) {
  if (!MR)
    return State;
  State = State->set<PossibleNullPtrMap>(MR, true);
  if (const auto *Alias = State->get<PtrAliasMap>(MR))
    State = State->set<PossibleNullPtrMap>(*Alias, true);
  return State;
}

static bool isDirectNullTestOperand(const Stmt *S, ASTContext &Ctx) {
  const Stmt *Current = S;
  while (Current) {
    const auto Parents = Ctx.getParents(*Current);
    const DynTypedNode *OnlyParent = nullptr;
    for (const DynTypedNode &Parent : Parents) {
      if (OnlyParent)
        return false;
      OnlyParent = &Parent;
    }
    if (!OnlyParent)
      return false;

    const Stmt *ParentStmt = OnlyParent->get<Stmt>();
    if (!ParentStmt)
      return false;
    if (isa<CastExpr>(ParentStmt) || isa<ParenExpr>(ParentStmt)) {
      Current = ParentStmt;
      continue;
    }
    if (const auto *UO = dyn_cast<UnaryOperator>(ParentStmt))
      return UO->getOpcode() == UO_LNot;
    if (const auto *BO = dyn_cast<BinaryOperator>(ParentStmt)) {
      if (BO->getOpcode() != BO_EQ && BO->getOpcode() != BO_NE)
        return false;
      return BO->getLHS()->isNullPointerConstant(
                 Ctx, Expr::NPC_ValueDependentIsNull) ||
             BO->getRHS()->isNullPointerConstant(
                 Ctx, Expr::NPC_ValueDependentIsNull);
    }
    return false;
  }
  return false;
}

namespace {

class SAGenTestChecker : public Checker<
    check::PostCall,        // For intercepting devm_kasprintf return values.
    check::BranchCondition, // For detecting null-checks.
    check::Bind,            // For propagating the pointer's null-check state.
    check::Location,        // For catching pointer dereferences.
    eval::Assume            // For applying null checks to their proven successor.
    > {
  mutable std::unique_ptr<BugType> BT;
  
public:
  SAGenTestChecker() : BT(new BugType(this, "Unchecked devm_kasprintf return", "Null Dereference")) {}

  // Callback: After a function call is evaluated.
  void checkPostCall(const CallEvent &Call, CheckerContext &C) const;

  // Callback: When an assignment (binding) occurs.
  void checkBind(SVal Loc, SVal Val, const Stmt *StoreE, CheckerContext &C) const;

  // Callback: When a branch condition is evaluated.
  void checkBranchCondition(const Stmt *Condition, CheckerContext &C) const;

  // Callback: When a memory location is accessed (load or store).
  void checkLocation(SVal Loc, bool IsLoad, const Stmt *S, CheckerContext &C) const;

  ProgramStateRef evalAssume(ProgramStateRef State, SVal Cond,
                             bool) const;

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
void SAGenTestChecker::checkBranchCondition(const Stmt *Condition, CheckerContext &C) const {
  ProgramStateRef State = C.getState();
  const Expr *CondE = dyn_cast<Expr>(Condition);
  if (!CondE) {
    C.addTransition(State);
    return;
  }

  auto scheduleNullCheck = [&](const MemRegion *MR) {
    if (!MR)
      return;
    MR = MR->getBaseRegion();
    if (State->get<PossibleNullPtrMap>(MR))
      State = State->set<PendingNullCheck>(MR, true);
  };

  CondE = CondE->IgnoreParenCasts();
  if (const auto *UO = dyn_cast<UnaryOperator>(CondE)) {
    if (UO->getOpcode() == UO_LNot) {
      SVal SubVal = C.getState()->getSVal(UO->getSubExpr(),
                                           C.getLocationContext());
      scheduleNullCheck(SubVal.getAsRegion());
    }
  } else if (const auto *BO = dyn_cast<BinaryOperator>(CondE)) {
    if (BO->getOpcode() == BO_EQ || BO->getOpcode() == BO_NE) {
      const Expr *LHS = BO->getLHS()->IgnoreParenCasts();
      const Expr *RHS = BO->getRHS()->IgnoreParenCasts();
      const bool LHSIsNull = LHS->isNullPointerConstant(
          C.getASTContext(), Expr::NPC_ValueDependentIsNull);
      const bool RHSIsNull = RHS->isNullPointerConstant(
          C.getASTContext(), Expr::NPC_ValueDependentIsNull);
      if (LHSIsNull != RHSIsNull) {
        const Expr *PtrExpr = LHSIsNull ? RHS : LHS;
        SVal PtrVal = C.getState()->getSVal(PtrExpr, C.getLocationContext());
        scheduleNullCheck(PtrVal.getAsRegion());
      }
    }
  } else {
    SVal CondVal = C.getState()->getSVal(CondE, C.getLocationContext());
    scheduleNullCheck(CondVal.getAsRegion());
  }

  C.addTransition(State);
}

ProgramStateRef SAGenTestChecker::evalAssume(ProgramStateRef State, SVal Cond,
                                             bool) const {
  const auto Pending = State->get<PendingNullCheck>();
  for (const auto &Entry : Pending) {
    const MemRegion *MR = Entry.first;
    State = State->remove<PendingNullCheck>(MR);
    SVal PtrVal = State->getSVal(MR);
    if (auto DefinedPtr = PtrVal.getAs<DefinedSVal>()) {
      if (!State->assume(*DefinedPtr, false))
        State = setChecked(State, MR);
    }
  }
  return State;
}

/// checkLocation - When a memory location is accessed (load or store), check if
/// it involves a pointer from devm_kasprintf that has not been NULL-checked.
void SAGenTestChecker::checkLocation(SVal Loc, bool IsLoad, const Stmt *S, CheckerContext &C) const {
  if (S && isDirectNullTestOperand(S, C.getASTContext()))
    return;

  ProgramStateRef State = C.getState();
  if (const MemRegion *MR = Loc.getAsRegion()) {
    MR = MR->getBaseRegion();
    if (!MR)
      return;
    
    const bool *Checked = State->get<PossibleNullPtrMap>(MR);
    // If the pointer originated from devm_kasprintf and remains unchecked, report it.
    if (Checked && !(*Checked)) {
      reportUncheckedUse(S, C);
    }
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
