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

REGISTER_SET_WITH_PROGRAMSTATE(PendingConditionalFieldOwnerSet, const MemRegion *)
REGISTER_TRAIT_WITH_PROGRAMSTATE(ReachedOwnerUseBarrier, bool)

namespace {

class SAGenTestChecker
    : public Checker<check::PostCall, check::PreCall> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker() : BT(new BugType(this, "Incorrect free in error path")) {}

  void checkPostCall(const CallEvent &Call, CheckerContext &C) const;
  void checkPreCall(const CallEvent &Call, CheckerContext &C) const;

private:
  const MemRegion *getBaseObjectRegion(const Expr *E, CheckerContext &C) const;
  const MemRegion *getFreedFieldBase(const CallEvent &Call, CheckerContext &C) const;
  void reportIncorrectFree(const CallEvent &Call, CheckerContext &C,
                           StringRef Msg) const;
};

const MemRegion *SAGenTestChecker::getBaseObjectRegion(const Expr *E,
                                                       CheckerContext &C) const {
  if (!E)
    return nullptr;
  E = E->IgnoreParenImpCasts();
  const MemRegion *MR = getMemRegionFromExpr(E, C);
  return MR ? MR->getBaseRegion() : nullptr;
}

const MemRegion *SAGenTestChecker::getFreedFieldBase(const CallEvent &Call,
                                                     CheckerContext &C) const {
  if (Call.getNumArgs() < 1)
    return nullptr;

  const Expr *Arg = Call.getArgExpr(0);
  if (!Arg)
    return nullptr;
  Arg = Arg->IgnoreParenImpCasts();

  const auto *ME = dyn_cast<MemberExpr>(Arg);
  if (!ME)
    return nullptr;

  return getBaseObjectRegion(ME->getBase(), C);
}

void SAGenTestChecker::checkPostCall(const CallEvent &Call, CheckerContext &C) const {
  const Expr *OriginExpr = Call.getOriginExpr();
  if (!OriginExpr)
    return;

  ProgramStateRef State = C.getState();

  if (ExprHasName(OriginExpr, "hws_definer_conv_match_params_to_hl", C) &&
      Call.getNumArgs() > 1) {
    if (const MemRegion *Owner = getBaseObjectRegion(Call.getArgExpr(1), C)) {
      State = State->add<PendingConditionalFieldOwnerSet>(Owner);
      State = State->set<ReachedOwnerUseBarrier>(false);
      C.addTransition(State);
    }
    return;
  }

  if (ExprHasName(OriginExpr, "hws_definer_find_best_match_fit", C)) {
    State = State->set<ReachedOwnerUseBarrier>(true);
    C.addTransition(State);
  }
}

void SAGenTestChecker::checkPreCall(const CallEvent &Call, CheckerContext &C) const {
  const Expr *OriginExpr = Call.getOriginExpr();
  if (!OriginExpr || !ExprHasName(OriginExpr, "kfree", C) || Call.getNumArgs() < 1)
    return;

  ProgramStateRef State = C.getState();

  if (const MemRegion *Owner = getFreedFieldBase(Call, C)) {
    if (State->contains<PendingConditionalFieldOwnerSet>(Owner) &&
        !State->get<ReachedOwnerUseBarrier>()) {
      reportIncorrectFree(
          Call, C,
          "error path frees a conditionally-owned field before the ownership barrier");
      return;
    }
  }

}

void SAGenTestChecker::reportIncorrectFree(const CallEvent &Call, CheckerContext &C,
                                           StringRef Msg) const {
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;
  auto report = std::make_unique<PathSensitiveBugReport>(*BT, Msg, N);
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
