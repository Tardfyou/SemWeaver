// /external/upstream-study/SAGEN/result-0224-bugfail-multi-o3mini/test-Double-Free-d8c561741ef83980114b3f7f95ffac54600f3f16/checkers/checker2.cpp
#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramState.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramStateTrait.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"

using namespace clang;
using namespace ento;

REGISTER_MAP_WITH_PROGRAMSTATE(SqReadyStatus, SymbolRef, const VarDecl *)
REGISTER_SET_WITH_PROGRAMSTATE(SqClosedOnReadyFailure, SymbolRef)

namespace {

class SAGenTestChecker
    : public Checker<check::PostCall, check::PreCall, check::PreStmt<ReturnStmt>> {
  mutable std::unique_ptr<BugType> BT;

  static const VarDecl *getBaseVariable(const Expr *E) {
    E = E->IgnoreParenImpCasts();
    while (const auto *Member = dyn_cast<MemberExpr>(E))
      E = Member->getBase()->IgnoreParenImpCasts();
    if (const auto *Ref = dyn_cast<DeclRefExpr>(E))
      return dyn_cast<VarDecl>(Ref->getDecl());
    return nullptr;
  }

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Double-free after failed SQ readiness transition")) {}

  void checkPostCall(const CallEvent &Call, CheckerContext &C) const;
  void checkPreCall(const CallEvent &Call, CheckerContext &C) const;
  void checkPreStmt(const ReturnStmt *RS, CheckerContext &C) const;
};

void SAGenTestChecker::checkPostCall(const CallEvent &Call,
                                     CheckerContext &C) const {
  const auto *Callee = dyn_cast_or_null<FunctionDecl>(Call.getDecl());
  if (!Callee || Callee->getName() != "hws_send_ring_set_sq_rdy" ||
      Call.getNumArgs() != 2)
    return;

  const VarDecl *Sq = getBaseVariable(Call.getArgExpr(1));
  SymbolRef Status = Call.getReturnValue().getAsSymbol();
  if (!Sq || !Status)
    return;

  C.addTransition(C.getState()->set<SqReadyStatus>(Status, Sq));
}

void SAGenTestChecker::checkPreCall(const CallEvent &Call,
                                    CheckerContext &C) const {
  const auto *Callee = dyn_cast_or_null<FunctionDecl>(Call.getDecl());
  if (!Callee || Callee->getName() != "hws_send_ring_close_sq" ||
      Call.getNumArgs() != 1)
    return;

  const VarDecl *Sq = getBaseVariable(Call.getArgExpr(0));
  if (!Sq)
    return;

  ProgramStateRef State = C.getState();
  for (const auto &Entry : State->get<SqReadyStatus>()) {
    SymbolRef Status = Entry.first;
    if (Entry.second != Sq)
      continue;

    // The zero alternative is infeasible only on the readiness-failure path.
    if (State->assume(nonloc::SymbolVal(Status), false))
      continue;

    C.addTransition(State->add<SqClosedOnReadyFailure>(Status));
    return;
  }
}

void SAGenTestChecker::checkPreStmt(const ReturnStmt *RS,
                                    CheckerContext &C) const {
  const Expr *Value = RS->getRetValue();
  if (!Value)
    return;

  SymbolRef Status = C.getSVal(Value).getAsSymbol();
  ProgramStateRef State = C.getState();
  if (!Status || !State->contains<SqClosedOnReadyFailure>(Status))
    return;

  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT,
      "SQ software resources are closed after readiness failure and the same error is propagated",
      N);
  Report->addRange(Value->getSourceRange());
  C.emitReport(std::move(Report));
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects SQ cleanup that is duplicated after readiness failure", "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
