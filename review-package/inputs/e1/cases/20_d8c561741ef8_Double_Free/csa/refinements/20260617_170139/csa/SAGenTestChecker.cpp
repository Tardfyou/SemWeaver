// /external/upstream-study/SAGEN/result-0224-bugfail-multi-o3mini/test-Double-Free-d8c561741ef83980114b3f7f95ffac54600f3f16/checkers/checker2.cpp
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"

using namespace clang;
using namespace ento;

namespace {

class SAGenTestChecker : public Checker<check::PreCall> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this,
                       "Double-free after failed SQ ready transition")) {}

  void checkPreCall(const CallEvent &Call, CheckerContext &C) const;
};

static bool isNamedFunction(const FunctionDecl *FD, StringRef Name) {
  return FD && FD->getIdentifier() && FD->getName() == Name;
}

static bool currentFunctionIs(CheckerContext &C, StringRef Name) {
  const Decl *D = C.getLocationContext()->getDecl();
  const auto *FD = dyn_cast_or_null<FunctionDecl>(D);
  return isNamedFunction(FD, Name);
}

static bool argumentLooksLikeSqObject(const Expr *E) {
  E = E ? E->IgnoreParenImpCasts() : nullptr;
  const auto *DRE = dyn_cast_or_null<DeclRefExpr>(E);
  return DRE && DRE->getDecl() && DRE->getDecl()->getName() == "sq";
}

void SAGenTestChecker::checkPreCall(const CallEvent &Call,
                                    CheckerContext &C) const {
  const auto *FD = dyn_cast_or_null<FunctionDecl>(Call.getDecl());
  if (!isNamedFunction(FD, "hws_send_ring_close_sq"))
    return;

  if (!currentFunctionIs(C, "hws_send_ring_create_sq_rdy"))
    return;

  if (Call.getNumArgs() != 1 || !argumentLooksLikeSqObject(Call.getArgExpr(0)))
    return;

  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT,
      "Double-free risk: failed SQ ready transition uses full software cleanup",
      N);
  Report->addRange(Call.getSourceRange());
  C.emitReport(std::move(Report));
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects hws_send_ring_close_sq on failed SQ ready transition",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
