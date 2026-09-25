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
REGISTER_MAP_WITH_PROGRAMSTATE(PossibleNullPtrMap, SymbolRef, bool)
REGISTER_MAP_WITH_PROGRAMSTATE(PtrAliasMap, const MemRegion*, SymbolRef)
REGISTER_MAP_WITH_PROGRAMSTATE(PendingNullCheck, SymbolRef, bool)

//------------------------------------------------------------------------------
// Helper functions for preserving the identity of a nullable pointer value.
//------------------------------------------------------------------------------
static ProgramStateRef setChecked(ProgramStateRef State, SymbolRef Sym) {
  if (!Sym)
    return State;
  return State->set<PossibleNullPtrMap>(Sym, true);
}

static SymbolRef getTrackedSymbol(SVal Value, ProgramStateRef State) {
  if (SymbolRef Sym = Value.getAsSymbol())
    return Sym;

  const MemRegion *MR = Value.getAsRegion();
  if (!MR)
    return nullptr;
  MR = MR->getBaseRegion();
  if (const auto *Alias = State->get<PtrAliasMap>(MR))
    return *Alias;
  if (const auto *Symbolic = dyn_cast<SymbolicRegion>(MR))
    return Symbolic->getSymbol();
  return nullptr;
}

static SymbolRef getDereferencedSymbol(SVal Loc) {
  const MemRegion *MR = Loc.getAsRegion();
  if (!MR)
    return nullptr;
  MR = MR->getBaseRegion();
  if (const auto *Symbolic = dyn_cast<SymbolicRegion>(MR))
    return Symbolic->getSymbol();
  return nullptr;
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
  if (!OriginExpr || !ExprHasName(OriginExpr, "devm_kasprintf", C))
    return;

  SymbolRef Sym = Call.getReturnValue().getAsSymbol();
  if (!Sym)
    return;

  ProgramStateRef State = C.getState();
  State = State->set<PossibleNullPtrMap>(Sym, false);
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

  SymbolRef RHS = getTrackedSymbol(Val, State);
  if (!RHS || !State->get<PossibleNullPtrMap>(RHS))
    return;

  State = State->set<PtrAliasMap>(LHSReg, RHS);
  C.addTransition(State);
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

  auto scheduleNullCheck = [&](SVal Value) {
    SymbolRef Sym = getTrackedSymbol(Value, State);
    if (Sym && State->get<PossibleNullPtrMap>(Sym))
      State = State->set<PendingNullCheck>(Sym, true);
  };

  CondE = CondE->IgnoreParenCasts();
  if (const auto *UO = dyn_cast<UnaryOperator>(CondE)) {
    if (UO->getOpcode() == UO_LNot) {
      SVal SubVal = C.getState()->getSVal(UO->getSubExpr(),
                                           C.getLocationContext());
      scheduleNullCheck(SubVal);
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
        scheduleNullCheck(PtrVal);
      }
    }
  } else {
    SVal CondVal = C.getState()->getSVal(CondE, C.getLocationContext());
    scheduleNullCheck(CondVal);
  }

  C.addTransition(State);
}

ProgramStateRef SAGenTestChecker::evalAssume(ProgramStateRef State, SVal Cond,
                                             bool) const {
  const auto Pending = State->get<PendingNullCheck>();
  for (const auto &Entry : Pending) {
    SymbolRef Sym = Entry.first;
    State = State->remove<PendingNullCheck>(Sym);
    nonloc::SymbolVal PtrVal(Sym);
    if (!State->assume(PtrVal, false))
      State = setChecked(State, Sym);
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
