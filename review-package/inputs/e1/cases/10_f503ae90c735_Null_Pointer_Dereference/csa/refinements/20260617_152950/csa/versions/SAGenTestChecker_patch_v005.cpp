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

namespace {

const MemRegion *getBase(const MemRegion *MR) {
  return MR ? MR->getBaseRegion() : nullptr;
}

const MemRegion *resolveTrackedRegion(ProgramStateRef State, const MemRegion *MR) {
  MR = getBase(MR);
  for (unsigned I = 0; MR && I < 4; ++I) {
    if (State->get<PossibleNullPtrMap>(MR))
      return MR;
    const auto *Root = State->get<PtrAliasMap>(MR);
    if (!Root)
      return nullptr;
    MR = getBase(*Root);
  }
  return nullptr;
}

class TrackedRegionFinder : public RecursiveASTVisitor<TrackedRegionFinder> {
  ProgramStateRef State;
  CheckerContext &C;
  bool RequireUnchecked;
  const MemRegion *Found = nullptr;

public:
  TrackedRegionFinder(ProgramStateRef State, CheckerContext &C,
                      bool RequireUnchecked)
      : State(State), C(C), RequireUnchecked(RequireUnchecked) {}

  bool VisitExpr(Expr *E) {
    if (Found)
      return false;
    SVal V = State->getSVal(E->IgnoreParenCasts(), C.getLocationContext());
    const MemRegion *Root = resolveTrackedRegion(State, V.getAsRegion());
    if (!Root)
      return true;
    const bool *Checked = State->get<PossibleNullPtrMap>(Root);
    if (!RequireUnchecked || (Checked && !*Checked))
      Found = Root;
    return !Found;
  }

  const MemRegion *getFound() const { return Found; }
};

const MemRegion *findTrackedRegionInStmt(const Stmt *S, ProgramStateRef State,
                                         CheckerContext &C,
                                         bool RequireUnchecked) {
  if (!S)
    return nullptr;
  TrackedRegionFinder Finder(State, C, RequireUnchecked);
  Finder.TraverseStmt(const_cast<Stmt *>(S));
  return Finder.getFound();
}

const MemRegion *findRegionCheckedByCondition(const Expr *CondE,
                                              ProgramStateRef State,
                                              CheckerContext &C) {
  if (!CondE)
    return nullptr;
  CondE = CondE->IgnoreParenCasts();

  if (const auto *UO = dyn_cast<UnaryOperator>(CondE)) {
    if (UO->getOpcode() == UO_LNot) {
      SVal SubVal = State->getSVal(UO->getSubExpr()->IgnoreParenCasts(),
                                   C.getLocationContext());
      return resolveTrackedRegion(State, SubVal.getAsRegion());
    }
  }

  if (const auto *BO = dyn_cast<BinaryOperator>(CondE)) {
    BinaryOperator::Opcode Op = BO->getOpcode();
    if (Op == BO_EQ || Op == BO_NE) {
      const Expr *LHS = BO->getLHS()->IgnoreParenCasts();
      const Expr *RHS = BO->getRHS()->IgnoreParenCasts();
      bool LHSIsNull = LHS->isNullPointerConstant(
          C.getASTContext(), Expr::NPC_ValueDependentIsNull);
      bool RHSIsNull = RHS->isNullPointerConstant(
          C.getASTContext(), Expr::NPC_ValueDependentIsNull);
      const Expr *PtrExpr = nullptr;
      if (LHSIsNull && !RHSIsNull)
        PtrExpr = RHS;
      else if (RHSIsNull && !LHSIsNull)
        PtrExpr = LHS;
      if (PtrExpr) {
        SVal PtrVal = State->getSVal(PtrExpr, C.getLocationContext());
        return resolveTrackedRegion(State, PtrVal.getAsRegion());
      }
    }
  }

  SVal CondVal = State->getSVal(CondE, C.getLocationContext());
  return resolveTrackedRegion(State, CondVal.getAsRegion());
}

bool isAddressFormation(const Stmt *S, CheckerContext &C) {
  const Stmt *Cur = S;
  for (unsigned I = 0; Cur && I < 6; ++I) {
    auto Parents = C.getASTContext().getParents(*Cur);
    if (Parents.empty())
      return false;
    const Stmt *Parent = Parents[0].get<Stmt>();
    if (!Parent)
      return false;
    if (const auto *UO = dyn_cast<UnaryOperator>(Parent))
      if (UO->getOpcode() == UO_AddrOf)
        return true;
    Cur = Parent;
  }
  return false;
}

bool isNullCheckConditionForRoot(const Stmt *S, const MemRegion *Root,
                                 ProgramStateRef State, CheckerContext &C) {
  const auto *If = findSpecificTypeInParents<IfStmt>(S, C);
  if (!If)
    return false;
  const MemRegion *CheckedRoot =
      findRegionCheckedByCondition(If->getCond(), State, C);
  return CheckedRoot && CheckedRoot == Root;
}

bool isImmediateExitStmt(const Stmt *S) {
  if (!S)
    return false;
  if (isa<ReturnStmt>(S))
    return true;
  if (const auto *CS = dyn_cast<CompoundStmt>(S)) {
    if (CS->body_empty())
      return false;
    auto It = CS->body_begin();
    const Stmt *Only = *It++;
    return It == CS->body_end() && isa<ReturnStmt>(Only);
  }
  return false;
}

bool conditionTrueMeansNullForRoot(const Expr *CondE, const MemRegion *Root,
                                   ProgramStateRef State, CheckerContext &C) {
  if (!CondE || !Root)
    return false;
  CondE = CondE->IgnoreParenCasts();

  if (const auto *UO = dyn_cast<UnaryOperator>(CondE)) {
    if (UO->getOpcode() == UO_LNot) {
      SVal SubVal = State->getSVal(UO->getSubExpr()->IgnoreParenCasts(),
                                   C.getLocationContext());
      return resolveTrackedRegion(State, SubVal.getAsRegion()) == Root;
    }
  }

  if (const auto *BO = dyn_cast<BinaryOperator>(CondE)) {
    BinaryOperator::Opcode Op = BO->getOpcode();
    if (Op == BO_EQ || Op == BO_NE) {
      const Expr *LHS = BO->getLHS()->IgnoreParenCasts();
      const Expr *RHS = BO->getRHS()->IgnoreParenCasts();
      bool LHSIsNull = LHS->isNullPointerConstant(
          C.getASTContext(), Expr::NPC_ValueDependentIsNull);
      bool RHSIsNull = RHS->isNullPointerConstant(
          C.getASTContext(), Expr::NPC_ValueDependentIsNull);
      const Expr *PtrExpr = nullptr;
      if (LHSIsNull && !RHSIsNull)
        PtrExpr = RHS;
      else if (RHSIsNull && !LHSIsNull)
        PtrExpr = LHS;
      if (!PtrExpr)
        return false;
      SVal PtrVal = State->getSVal(PtrExpr, C.getLocationContext());
      return resolveTrackedRegion(State, PtrVal.getAsRegion()) == Root &&
             Op == BO_EQ;
    }
  }

  return false;
}

ProgramStateRef setChecked(ProgramStateRef State, const MemRegion *MR) {
  const MemRegion *Root = resolveTrackedRegion(State, MR);
  if (!Root)
    return State;
  const bool *Checked = State->get<PossibleNullPtrMap>(Root);
  if (Checked && !*Checked)
    State = State->set<PossibleNullPtrMap>(Root, true);
  return State;
}

/// The checker class.
class SAGenTestChecker
  : public Checker< check::PostCall,
                    check::BranchCondition,
                    check::Location,
                    check::Bind > {
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
  
  // Track the nullable return value itself; later binds connect local aliases to it.
  const MemRegion *MR = getBase(Call.getReturnValue().getAsRegion());
  if (!MR)
    return;

  State = State->set<PossibleNullPtrMap>(MR, false);
  C.addTransition(State);
}

/// checkBranchCondition
/// Mark a tracked nullable pointer as guarded only when the condition rejects
/// the NULL case with an immediate return, matching the patch's barrier.
void SAGenTestChecker::checkBranchCondition(const Stmt *Condition, CheckerContext &C) const {
  ProgramStateRef State = C.getState();
  const Expr *CondE = dyn_cast<Expr>(Condition);
  if (!CondE) {
    C.addTransition(State);
    return;
  }
  CondE = CondE->IgnoreParenCasts();

  const auto *If = findSpecificTypeInParents<IfStmt>(CondE, C);
  const MemRegion *Root = findRegionCheckedByCondition(CondE, State, C);
  if (If && Root && conditionTrueMeansNullForRoot(CondE, Root, State, C) &&
      isImmediateExitStmt(If->getThen()))
    State = setChecked(State, Root);

  C.addTransition(State);
}

/// checkLocation
/// This callback is triggered on a load (i.e. a dereference). If the memory location
/// being loaded is derived from a pointer that was never null-checked (i.e. marked false
/// in PossibleNullPtrMap), emit a bug report.
void SAGenTestChecker::checkLocation(SVal Loc, bool isLoad, const Stmt *S, CheckerContext &C) const {
  // We are interested only in real loads, not address formation or guard tests.
  if (!isLoad)
    return;
  ProgramStateRef State = C.getState();
  const MemRegion *Root = findTrackedRegionInStmt(S, State, C, true);
  if (!Root)
    Root = resolveTrackedRegion(State, Loc.getAsRegion());
  const bool *Checked = Root ? State->get<PossibleNullPtrMap>(Root) : nullptr;
  if (!Checked || *Checked)
    return;

  if (isAddressFormation(S, C) || isNullCheckConditionForRoot(S, Root, State, C))
    return;

  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;
  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT,
      "Missing NULL check for mt76_connac_get_he_phy_cap return value", N);
  C.emitReport(std::move(Report));
}

/// checkBind
/// This callback tracks pointer aliasing. When one pointer is assigned to another,
/// record their relationship so that if one is marked as checked, its alias is updated.
void SAGenTestChecker::checkBind(SVal Loc, SVal Val, const Stmt *StoreE, CheckerContext &C) const {
  ProgramStateRef State = C.getState();
  const MemRegion *LHS = getBase(Loc.getAsRegion());
  if (!LHS)
    return;
  const MemRegion *Root = resolveTrackedRegion(State, Val.getAsRegion());
  if (!Root)
    Root = findTrackedRegionInStmt(StoreE, State, C, false);
  if (!Root)
    return;
  State = State->set<PtrAliasMap>(LHS, Root);
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
