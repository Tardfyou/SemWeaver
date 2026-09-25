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

static const MemRegion *getBaseRegionFromSVal(SVal V) {
  const MemRegion *MR = V.getAsRegion();
  return MR ? MR->getBaseRegion() : nullptr;
}

static const bool *getTrackedStateForRegion(ProgramStateRef State,
                                            const MemRegion *MR) {
  if (!MR)
    return nullptr;
  MR = MR->getBaseRegion();
  if (!MR)
    return nullptr;
  if (const bool *Checked = State->get<PossibleNullPtrMap>(MR))
    return Checked;
  if (const auto *Alias = State->get<PtrAliasMap>(MR))
    return State->get<PossibleNullPtrMap>(*Alias);
  return nullptr;
}

static bool isTrackedUnchecked(ProgramStateRef State, SVal V) {
  const bool *Checked = getTrackedStateForRegion(State, getBaseRegionFromSVal(V));
  return Checked && !*Checked;
}

static bool isPersistentMemberStore(const MemRegion *MR) {
  return MR && isa<FieldRegion>(MR);
}

static bool isExplicitDereferenceUse(const Stmt *S) {
  const auto *E = dyn_cast_or_null<Expr>(S);
  if (!E)
    return false;
  E = E->IgnoreParenCasts();
  if (const auto *UO = dyn_cast<UnaryOperator>(E))
    return UO->getOpcode() == UO_Deref;
  if (isa<ArraySubscriptExpr>(E))
    return true;
  if (const auto *ME = dyn_cast<MemberExpr>(E))
    return ME->isArrow();
  return false;
}

namespace {

class SAGenTestChecker : public Checker<
    check::PostCall,        // For intercepting nullable formatted allocation returns.
    check::PreCall,         // For catching unchecked values passed to dereferencing callees.
    check::BranchCondition, // For detecting null-checks.
    check::Bind,            // For propagation and object-field escapes.
    check::Location         // For catching explicit pointer dereferences.
    > {
  mutable std::unique_ptr<BugType> BT;
  
public:
  SAGenTestChecker() : BT(new BugType(this, "Unchecked devm_kasprintf return", "Null Dereference")) {}

  // Callback: After a function call is evaluated.
  void checkPostCall(const CallEvent &Call, CheckerContext &C) const;

  // Callback: Before a function call is evaluated.
  void checkPreCall(const CallEvent &Call, CheckerContext &C) const;

  // Callback: When an assignment (binding) occurs.
  void checkBind(SVal Loc, SVal Val, const Stmt *StoreE, CheckerContext &C) const;

  // Callback: When a branch condition is evaluated.
  void checkBranchCondition(const Stmt *Condition, CheckerContext &C) const;

  // Callback: When a memory location is accessed (load or store).
  void checkLocation(SVal Loc, bool IsLoad, const Stmt *S, CheckerContext &C) const;

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

/// checkPreCall - Report unchecked nullable allocation values passed to callees
/// whose contracts are known to dereference those parameters.
void SAGenTestChecker::checkPreCall(const CallEvent &Call, CheckerContext &C) const {
  llvm::SmallVector<unsigned, 4> DerefParams;
  if (!functionKnownToDeref(Call, DerefParams))
    return;

  ProgramStateRef State = C.getState();
  for (unsigned Idx : DerefParams) {
    if (Idx >= Call.getNumArgs())
      continue;
    if (isTrackedUnchecked(State, Call.getArgSVal(Idx)))
      reportUncheckedUse(Call.getArgExpr(Idx), C);
  }
}

/// checkBind - Propagate null-check state through pointer aliases and report
/// unchecked nullable allocation values that escape into object fields.
void SAGenTestChecker::checkBind(SVal Loc, SVal Val, const Stmt *StoreE, CheckerContext &C) const {
  ProgramStateRef State = C.getState();
  const MemRegion *LHSRawReg = Loc.getAsRegion();
  if (!LHSRawReg)
    return;
  const MemRegion *LHSReg = LHSRawReg->getBaseRegion();
  const MemRegion *RHSReg = getBaseRegionFromSVal(Val);
  if (!LHSReg || !RHSReg)
    return;

  const bool *RHSChecked = getTrackedStateForRegion(State, RHSReg);
  if (!RHSChecked)
    return;

  if (!*RHSChecked && isPersistentMemberStore(LHSRawReg))
    reportUncheckedUse(StoreE, C);

  State = State->set<PossibleNullPtrMap>(LHSReg, *RHSChecked);
  State = State->set<PtrAliasMap>(LHSReg, RHSReg);
  State = State->set<PtrAliasMap>(RHSReg, LHSReg);
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
  
  // Remove parentheses and casts.
  CondE = CondE->IgnoreParenCasts();
  
  // Case 1: if (!ptr)
  if (const auto *UO = dyn_cast<UnaryOperator>(CondE)) {
    if (UO->getOpcode() == UO_LNot) {
      const Expr *SubE = UO->getSubExpr();
      SVal SubVal = C.getState()->getSVal(SubE, C.getLocationContext());
      if (const MemRegion *MR = SubVal.getAsRegion()) {
        MR = MR->getBaseRegion();
        State = setChecked(State, MR);
      }
    }
  }
  // Case 2: if (ptr == NULL) or if (ptr != NULL)
  else if (const auto *BO = dyn_cast<BinaryOperator>(CondE)) {
    BinaryOperator::Opcode Opcode = BO->getOpcode();
    if (Opcode == BO_EQ || Opcode == BO_NE) {
      const Expr *LHS = BO->getLHS()->IgnoreParenCasts();
      const Expr *RHS = BO->getRHS()->IgnoreParenCasts();
      bool LHSIsNull = LHS->isNullPointerConstant(C.getASTContext(), Expr::NPC_ValueDependentIsNull);
      bool RHSIsNull = RHS->isNullPointerConstant(C.getASTContext(), Expr::NPC_ValueDependentIsNull);
      
      const Expr *PtrExpr = nullptr;
      if (LHSIsNull && !RHSIsNull)
        PtrExpr = RHS;
      else if (RHSIsNull && !LHSIsNull)
        PtrExpr = LHS;
      
      if (PtrExpr) {
        SVal PtrVal = C.getState()->getSVal(PtrExpr, C.getLocationContext());
        if (const MemRegion *MR = PtrVal.getAsRegion()) {
          MR = MR->getBaseRegion();
          State = setChecked(State, MR);
        }
      }
    }
  }
  // Case 3: if (ptr) : treat as non-zero check.
  else {
    SVal CondVal = C.getState()->getSVal(CondE, C.getLocationContext());
    if (const MemRegion *MR = CondVal.getAsRegion()) {
      MR = MR->getBaseRegion();
      State = setChecked(State, MR);
    }
  }
  
  C.addTransition(State);
}

/// checkLocation - Report only explicit dereference-like uses of an unchecked
/// nullable allocation result, not ordinary loads such as a NULL-test itself.
void SAGenTestChecker::checkLocation(SVal Loc, bool IsLoad, const Stmt *S, CheckerContext &C) const {
  if (!isExplicitDereferenceUse(S))
    return;

  ProgramStateRef State = C.getState();
  const bool *Checked = getTrackedStateForRegion(State, getBaseRegionFromSVal(Loc));
  if (Checked && !*Checked)
    reportUncheckedUse(S, C);
}

/// reportUncheckedUse - Generate a bug report when an unchecked devm_kasprintf
/// pointer is dereferenced.
void SAGenTestChecker::reportUncheckedUse(const Stmt *S, CheckerContext &C) const {
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;
  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT, "Unchecked devm_kasprintf return value used", N);
  if (S)
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
