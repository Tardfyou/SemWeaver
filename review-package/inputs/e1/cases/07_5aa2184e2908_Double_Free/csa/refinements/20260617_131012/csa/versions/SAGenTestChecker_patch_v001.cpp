// /external/upstream-study/SAGEN/result-0224-bugfail-multi-o3mini/test-Double-Free-5aa2184e29081665f915594bc6de9b7fee6e4883/checkers/checker0.cpp
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
#include "clang/AST/ExprCXX.h"

using namespace clang;
using namespace ento;
using namespace taint;

// Track resources that were acquired on the current path and are still owned.
REGISTER_MAP_WITH_PROGRAMSTATE(AllocatedRegionMap, const MemRegion *, bool)
// Track simple pointer copies so ownership can follow aliases.
REGISTER_MAP_WITH_PROGRAMSTATE(PtrAliasMap, const MemRegion *, const MemRegion *)
// Distinguish suspicious cleanup frees from ordinary releases of caller-owned memory.
REGISTER_TRAIT_WITH_PROGRAMSTATE(SawPathAllocation, bool)

namespace {

class SAGenTestChecker
    : public Checker<check::PostCall, check::PreCall, check::Bind> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker() : BT(new BugType(this, "Incorrect free in error path")) {}

  // Callback to track allocation calls.
  void checkPostCall(const CallEvent &Call, CheckerContext &C) const;
  // Callback to check free calls.
  void checkPreCall(const CallEvent &Call, CheckerContext &C) const;
  // Callback to track pointer assignments (for aliasing).
  void checkBind(SVal Loc, SVal Val, const Stmt *S, CheckerContext &C) const;

private:
  bool isKernelAllocator(const Expr *E, CheckerContext &C) const;
  bool isKernelDeallocator(const Expr *E, CheckerContext &C) const;
  const MemRegion *getTrackedRegion(SVal V) const;
  const MemRegion *resolveAlias(ProgramStateRef State, const MemRegion *MR) const;
  ProgramStateRef markAllocated(ProgramStateRef State, const MemRegion *MR) const;

  void reportIncorrectFree(const CallEvent &Call, CheckerContext &C,
                           const MemRegion *MR) const;
};

bool SAGenTestChecker::isKernelAllocator(const Expr *E, CheckerContext &C) const {
  return E && (ExprHasName(E, "kzalloc", C) || ExprHasName(E, "kmalloc", C) ||
               ExprHasName(E, "kcalloc", C) || ExprHasName(E, "kvzalloc", C) ||
               ExprHasName(E, "kvmalloc", C) || ExprHasName(E, "vmalloc", C));
}

bool SAGenTestChecker::isKernelDeallocator(const Expr *E, CheckerContext &C) const {
  return E && (ExprHasName(E, "kfree", C) || ExprHasName(E, "kvfree", C) ||
               ExprHasName(E, "vfree", C));
}

const MemRegion *SAGenTestChecker::getTrackedRegion(SVal V) const {
  const MemRegion *MR = V.getAsRegion();
  return MR ? MR->getBaseRegion() : nullptr;
}

const MemRegion *SAGenTestChecker::resolveAlias(ProgramStateRef State,
                                                const MemRegion *MR) const {
  if (!MR)
    return nullptr;
  const MemRegion *Current = MR;
  for (unsigned I = 0; I < 4; ++I) {
    const MemRegion *const *Next = State->get<PtrAliasMap>(Current);
    if (!Next || !*Next || *Next == Current)
      break;
    Current = (*Next)->getBaseRegion();
  }
  return Current;
}

ProgramStateRef SAGenTestChecker::markAllocated(ProgramStateRef State,
                                                const MemRegion *MR) const {
  if (!MR)
    return State;
  State = State->set<AllocatedRegionMap>(MR, true);
  State = State->set<PtrAliasMap>(MR, MR);
  return State->set<SawPathAllocation>(true);
}

void SAGenTestChecker::checkPostCall(const CallEvent &Call, CheckerContext &C) const {
  const Expr *OriginExpr = Call.getOriginExpr();
  if (!isKernelAllocator(OriginExpr, C))
    return;

  const MemRegion *MR = getTrackedRegion(Call.getReturnValue());
  if (!MR)
    return;

  C.addTransition(markAllocated(C.getState(), MR));
}

void SAGenTestChecker::checkPreCall(const CallEvent &Call, CheckerContext &C) const {
  const Expr *OriginExpr = Call.getOriginExpr();
  if (!isKernelDeallocator(OriginExpr, C))
    return;

  if (Call.getNumArgs() < 1)
    return;

  ProgramStateRef State = C.getState();
  const MemRegion *MR = resolveAlias(State, getTrackedRegion(Call.getArgSVal(0)));
  if (!MR)
    return;

  const bool *IsAllocated = State->get<AllocatedRegionMap>(MR);
  if (IsAllocated && *IsAllocated) {
    State = State->remove<AllocatedRegionMap>(MR);
    C.addTransition(State);
    return;
  }

  if (State->get<SawPathAllocation>())
    reportIncorrectFree(Call, C, MR);
}

void SAGenTestChecker::checkBind(SVal Loc, SVal Val, const Stmt *S, CheckerContext &C) const {
  const MemRegion *LHSReg = getTrackedRegion(Loc);
  if (!LHSReg)
    return;

  ProgramStateRef State = C.getState();
  const Expr *BoundExpr = dyn_cast_or_null<Expr>(S);
  const MemRegion *RHSReg = getTrackedRegion(Val);

  if (isKernelAllocator(BoundExpr, C)) {
    State = markAllocated(State, LHSReg);
    State = markAllocated(State, RHSReg);
    if (RHSReg)
      State = State->set<PtrAliasMap>(LHSReg, RHSReg);
    C.addTransition(State);
    return;
  }

  if (!RHSReg)
    return;

  RHSReg = resolveAlias(State, RHSReg);
  State = State->set<PtrAliasMap>(LHSReg, RHSReg);
  const bool *RHSAllocated = State->get<AllocatedRegionMap>(RHSReg);
  if (RHSAllocated && *RHSAllocated)
    State = State->set<AllocatedRegionMap>(LHSReg, true);
  C.addTransition(State);
}

void SAGenTestChecker::reportIncorrectFree(const CallEvent &Call, CheckerContext &C,
                                             const MemRegion *MR) const {
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;
  auto report = std::make_unique<PathSensitiveBugReport>(
      *BT, "Resource is freed on a path where this checker has not seen matching ownership acquisition", N);
  report->addRange(Call.getSourceRange());
  C.emitReport(std::move(report));
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker", 
      "Detects cleanup paths that free unallocated resources, which may indicate a double free error",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
