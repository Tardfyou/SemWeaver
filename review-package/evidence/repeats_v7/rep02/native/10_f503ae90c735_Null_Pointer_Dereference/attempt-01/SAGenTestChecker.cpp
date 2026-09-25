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

// This map records pointer symbols returned by the fallible producer and
// whether the current path has established that the pointer is non-null.
REGISTER_MAP_WITH_PROGRAMSTATE(PossibleNullPtrMap, SymbolRef, bool)
// These maps connect a branch-condition symbol with the tracked pointer it
// tests and the condition truth value that proves that pointer non-null.
REGISTER_MAP_WITH_PROGRAMSTATE(PendingNullGuardPtrMap, SymbolRef, SymbolRef)
REGISTER_MAP_WITH_PROGRAMSTATE(PendingNullGuardPolarityMap, SymbolRef, bool)

namespace {

ProgramStateRef setChecked(ProgramStateRef State, SymbolRef PtrSym) {
  if (!PtrSym)
    return State;
  const bool *Checked = State->get<PossibleNullPtrMap>(PtrSym);
  if (Checked && !*Checked)
    State = State->set<PossibleNullPtrMap>(PtrSym, true);
  return State;
}

ProgramStateRef recordPendingNullGuard(ProgramStateRef State,
                                       const Expr *Condition,
                                       const Expr *PtrExpr,
                                       bool NonNullWhenTrue,
                                       CheckerContext &C) {
  SVal ConditionVal = State->getSVal(Condition, C.getLocationContext());
  SVal PtrVal = State->getSVal(PtrExpr, C.getLocationContext());
  SymbolRef ConditionSym = ConditionVal.getAsSymbol();
  SymbolRef PtrSym = PtrVal.getAsSymbol();
  if (!ConditionSym || !PtrSym ||
      !State->get<PossibleNullPtrMap>(PtrSym))
    return State;

  State = State->set<PendingNullGuardPtrMap>(ConditionSym, PtrSym);
  return State->set<PendingNullGuardPolarityMap>(ConditionSym,
                                                   NonNullWhenTrue);
}

/// The checker class.
class SAGenTestChecker
  : public Checker< check::PostCall,
                    check::BranchCondition,
                    check::Location,
                    eval::Assume > {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker() 
    : BT(new BugType(this, "Missing NULL check for mt76_connac_get_he_phy_cap return value")) {}

  // Callback for recording the return value.
  void checkPostCall(const CallEvent &Call, CheckerContext &C) const;

  // Callback for marking that a pointer has been null-checked.
  void checkBranchCondition(const Stmt *Condition, CheckerContext &C) const;

  // Callback for detecting dereferences (i.e. loads) of unchecked pointers.
  void checkLocation(SVal Loc, bool isLoad, const Stmt *S, CheckerContext &C) const;

  ProgramStateRef evalAssume(ProgramStateRef State, SVal Cond,
                             bool Assumption) const;
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
  
  SymbolRef PtrSym = Call.getReturnValue().getAsSymbol();
  if (!PtrSym)
    return;

  // Record the returned pointer value until a path proves it non-null.
  State = State->set<PossibleNullPtrMap>(PtrSym, false);
  C.addTransition(State);
}

/// checkBranchCondition
/// This callback inspects branch conditions to see if the pointer is being
/// explicitly checked against NULL (via conditions like "if (!ptr)" or "if (ptr == NULL)").
void SAGenTestChecker::checkBranchCondition(const Stmt *Condition, CheckerContext &C) const {
  ProgramStateRef State = C.getState();
  const Expr *CondE = dyn_cast<Expr>(Condition);
  if (!CondE) {
    C.addTransition(State);
    return;
  }
  CondE = CondE->IgnoreParenCasts();

  const Expr *PtrExpr = nullptr;
  bool NonNullWhenTrue = true;
  if (const UnaryOperator *UO = dyn_cast<UnaryOperator>(CondE)) {
    if (UO->getOpcode() == UO_LNot) {
      PtrExpr = UO->getSubExpr()->IgnoreParenCasts();
      NonNullWhenTrue = false;
    }
  } else if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(CondE)) {
    BinaryOperator::Opcode Op = BO->getOpcode();
    if (Op == BO_EQ || Op == BO_NE) {
      const Expr *LHS = BO->getLHS()->IgnoreParenCasts();
      const Expr *RHS = BO->getRHS()->IgnoreParenCasts();
      bool LHSIsNull = LHS->isNullPointerConstant(
          C.getASTContext(), Expr::NPC_ValueDependentIsNull);
      bool RHSIsNull = RHS->isNullPointerConstant(
          C.getASTContext(), Expr::NPC_ValueDependentIsNull);
      if (LHSIsNull && !RHSIsNull)
        PtrExpr = RHS;
      else if (RHSIsNull && !LHSIsNull)
        PtrExpr = LHS;
      NonNullWhenTrue = Op == BO_NE;
    }
  } else {
    PtrExpr = CondE;
  }

  if (PtrExpr)
    State = recordPendingNullGuard(State, CondE, PtrExpr,
                                   NonNullWhenTrue, C);
  C.addTransition(State);
}

ProgramStateRef SAGenTestChecker::evalAssume(ProgramStateRef State, SVal Cond,
                                             bool Assumption) const {
  SymbolRef ConditionSym = Cond.getAsSymbol();
  if (!ConditionSym)
    return State;
  const SymbolRef *PtrSym = State->get<PendingNullGuardPtrMap>(ConditionSym);
  const bool *NonNullWhenTrue =
      State->get<PendingNullGuardPolarityMap>(ConditionSym);
  if (!PtrSym || !NonNullWhenTrue)
    return State;

  State = State->remove<PendingNullGuardPtrMap>(ConditionSym);
  State = State->remove<PendingNullGuardPolarityMap>(ConditionSym);
  if (Assumption == *NonNullWhenTrue)
    State = setChecked(State, *PtrSym);
  return State;
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
  SymbolRef PtrSym = Loc.getAsSymbol(true);
  if (!PtrSym)
    return;
  const bool *Checked = State->get<PossibleNullPtrMap>(PtrSym);
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

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker", 
      "Detects missing NULL check for the return value of mt76_connac_get_he_phy_cap", 
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
