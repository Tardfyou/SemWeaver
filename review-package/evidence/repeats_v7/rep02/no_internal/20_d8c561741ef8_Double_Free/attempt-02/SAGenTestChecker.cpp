// /external/upstream-study/SAGEN/result-0224-bugfail-multi-o3mini/test-Double-Free-d8c561741ef83980114b3f7f95ffac54600f3f16/checkers/checker2.cpp
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
#include "clang/AST/Expr.h"  // for Expr

using namespace clang;
using namespace ento;
using namespace taint;

REGISTER_MAP_WITH_PROGRAMSTATE(PendingSQReadyStatus, SymbolRef,
                               const MemRegion *)

namespace {

// A failed transition to SQ-ready leaves the SQ's software allocations owned
// by the caller.  The error path must destroy the hardware SQ before caller
// cleanup can release those allocations.
class SAGenTestChecker
    : public Checker<check::PreCall, check::PostCall, eval::Assume> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Double-free after failed SQ-ready transition")) {}

  void checkPreCall(const CallEvent &Call, CheckerContext &C) const;
  void checkPostCall(const CallEvent &Call, CheckerContext &C) const;
  ProgramStateRef evalAssume(ProgramStateRef State, SVal Cond,
                             bool Assumption) const;

private:
  static bool isNamedCall(const CallEvent &Call, StringRef Name);
  static const MemRegion *getSQRegion(const CallEvent &Call);
  static const MemRegion *getReadySQRegion(const CallEvent &Call,
                                           CheckerContext &C);
  static ProgramStateRef clearPendingSQ(ProgramStateRef State,
                                        const MemRegion *SQ);
};

bool SAGenTestChecker::isNamedCall(const CallEvent &Call, StringRef Name) {
  const IdentifierInfo *Identifier = Call.getCalleeIdentifier();
  return Identifier && Identifier->getName() == Name;
}

const MemRegion *SAGenTestChecker::getSQRegion(const CallEvent &Call) {
  if (Call.getNumArgs() == 0)
    return nullptr;

  const MemRegion *Region = Call.getArgSVal(0).getAsRegion();
  return Region ? Region->getBaseRegion() : nullptr;
}

const MemRegion *SAGenTestChecker::getReadySQRegion(
    const CallEvent &Call, CheckerContext &C) {
  const Expr *Origin = Call.getOriginExpr();
  const auto *ReadyCall = dyn_cast_or_null<CallExpr>(Origin);
  if (!ReadyCall || ReadyCall->getNumArgs() == 0)
    return nullptr;

  const Expr *ReadyNumber = ReadyCall->getArg(0)->IgnoreParenImpCasts();
  const auto *Member = dyn_cast<MemberExpr>(ReadyNumber);
  if (!Member || !Member->isArrow())
    return nullptr;

  const MemRegion *Region = C.getSVal(Member->getBase()).getAsRegion();
  return Region ? Region->getBaseRegion() : nullptr;
}

ProgramStateRef SAGenTestChecker::clearPendingSQ(ProgramStateRef State,
                                                 const MemRegion *SQ) {
  ProgramStateRef Result = State;
  auto Pending = State->get<PendingSQReadyStatus>();
  for (auto I = Pending.begin(), E = Pending.end(); I != E; ++I) {
    if (I->second == SQ)
      Result = Result->remove<PendingSQReadyStatus>(I->first);
  }
  return Result;
}

void SAGenTestChecker::checkPostCall(const CallEvent &Call,
                                     CheckerContext &C) const {
  if (!isNamedCall(Call, "hws_send_ring_set_sq_rdy"))
    return;

  const MemRegion *SQ = getReadySQRegion(Call, C);
  SymbolRef Status = Call.getReturnValue().getAsSymbol();
  if (!SQ || !Status)
    return;

  C.addTransition(C.getState()->set<PendingSQReadyStatus>(Status, SQ));
}

ProgramStateRef SAGenTestChecker::evalAssume(ProgramStateRef State, SVal Cond,
                                             bool Assumption) const {
  ProgramStateRef Result = State;
  auto Pending = State->get<PendingSQReadyStatus>();
  for (auto I = Pending.begin(), E = Pending.end(); I != E; ++I) {
    const llvm::APSInt *Value =
        State->getConstraintManager().getSymVal(State, I->first);
    if (Value && Value->isZero())
      Result = Result->remove<PendingSQReadyStatus>(I->first);
  }
  return Result;
}

void SAGenTestChecker::checkPreCall(const CallEvent &Call,
                                    CheckerContext &C) const {
  const MemRegion *SQ = getSQRegion(Call);
  if (!SQ)
    return;

  if (isNamedCall(Call, "hws_send_ring_destroy_sq")) {
    C.addTransition(clearPendingSQ(C.getState(), SQ));
    return;
  }

  if (!isNamedCall(Call, "hws_send_ring_close_sq"))
    return;

  auto Pending = C.getState()->get<PendingSQReadyStatus>();
  bool NeedsHardwareDestroy = false;
  for (auto I = Pending.begin(), E = Pending.end(); I != E; ++I) {
    if (I->second == SQ) {
      NeedsHardwareDestroy = true;
      break;
    }
  }
  if (!NeedsHardwareDestroy)
    return;

  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT,
      "SQ cleanup follows a failed ready transition without destroying the SQ",
      N);
  Report->addRange(Call.getSourceRange());
  C.emitReport(std::move(Report));
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker", 
      "Detects error path cleanup using wrong routine 'hws_send_ring_close_sq'", 
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
