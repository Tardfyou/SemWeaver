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

REGISTER_MAP_WITH_PROGRAMSTATE(PendingReadyTransition, SymbolRef,
                               const MemRegion *)

namespace {

// A failed ready-state transition leaves the hardware SQ needing hardware-only
// destruction; its containing software SQ must not be closed on that path.
class SAGenTestChecker
    : public Checker<check::PostCall, check::PreCall, eval::Assume> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Double-free during SQ error cleanup")) {}

  void checkPostCall(const CallEvent &Call, CheckerContext &C) const;
  void checkPreCall(const CallEvent &Call, CheckerContext &C) const;
  ProgramStateRef evalAssume(ProgramStateRef State, SVal Cond,
                             bool Assumption) const;
};

void SAGenTestChecker::checkPostCall(const CallEvent &Call,
                                     CheckerContext &C) const {
  if (!Call.getResultType()->isIntegralOrEnumerationType())
    return;

  SymbolRef Status = Call.getReturnValue().getAsSymbol();
  const auto *Origin = dyn_cast_or_null<CallExpr>(Call.getOriginExpr());
  if (!Status || !Origin)
    return;

  for (unsigned I = 0; I < Origin->getNumArgs(); ++I) {
    const auto *Member = dyn_cast<MemberExpr>(
        Origin->getArg(I)->IgnoreParenImpCasts());
    if (!Member || !Member->getType()->isIntegerType())
      continue;

    const Expr *Base = Member->getBase()->IgnoreParenImpCasts();
    const MemRegion *Object =
        C.getState()->getSVal(Base, C.getLocationContext()).getAsRegion();
    if (!Object)
      continue;

    C.addTransition(C.getState()->set<PendingReadyTransition>(Status, Object));
    return;
  }
}

ProgramStateRef SAGenTestChecker::evalAssume(ProgramStateRef State, SVal Cond,
                                             bool Assumption) const {
  SymbolRef Status = Cond.getAsSymbol();
  if (!Status || !State->get<PendingReadyTransition>().lookup(Status))
    return State;

  const llvm::APSInt *Value =
      State->getConstraintManager().getSymVal(State, Status);
  if (Value && Value->isZero())
    return State->remove<PendingReadyTransition>(Status);
  return State;
}

void SAGenTestChecker::checkPreCall(const CallEvent &Call,
                                    CheckerContext &C) const {
  const auto *Callee = dyn_cast_or_null<FunctionDecl>(Call.getDecl());
  if (!Callee || Callee->getName() != "hws_send_ring_close_sq" ||
      Call.getNumArgs() != 1)
    return;

  const MemRegion *Object = Call.getArgSVal(0).getAsRegion();
  if (!Object)
    return;

  ProgramStateRef State = C.getState();
  for (auto I = State->get<PendingReadyTransition>().begin(),
            E = State->get<PendingReadyTransition>().end();
       I != E; ++I) {
    if (I->second != Object)
      continue;

    DefinedSVal StatusValue =
        C.getSValBuilder().makeSymbolVal(I->first).castAs<DefinedSVal>();
    if (!State->assume(StatusValue, true))
      continue;

    ExplodedNode *N = C.generateNonFatalErrorNode();
    if (!N)
      return;

    auto Report = std::make_unique<PathSensitiveBugReport>(
        *BT, "Closing the software SQ after its ready transition failed can double free its resources", N);
    Report->addRange(Call.getSourceRange());
    C.emitReport(std::move(Report));
    return;
  }
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
