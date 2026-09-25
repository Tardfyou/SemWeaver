// /external/upstream-study/SAGEN/result-0224-bugfail-multi-o3mini/test-Uninit-Data-c48a4497356f701f94f1951626637ae240af909e/checkers/checker7.cpp
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Checkers/Taint.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/Environment.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ConstraintManager.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramState.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramStateTrait.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SymExpr.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtVisitor.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/StaticAnalyzer/Checkers/utility.h"
#include "clang/Lex/Lexer.h"

using namespace clang;
using namespace ento;
using namespace taint;

// Store each request_firmware() output slot with the status value that must
// be proven successful before the slot may be treated as initialized.
REGISTER_MAP_WITH_PROGRAMSTATE(RequestFwMap, const MemRegion*, SymbolRef)

namespace {

class SAGenTestChecker : public Checker< check::PostCall, check::BranchCondition,
                                        eval::Assume > {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker() : BT(new BugType(this, "Unchecked return value of request_firmware()")) {}

  // Callback to record the firmware pointer coming from the request_firmware() call.
  void checkPostCall(const CallEvent &Call, CheckerContext &C) const;

  // Callback to detect when the firmware pointer is directly checked in a branch condition.
  void checkBranchCondition(const Stmt *Condition, CheckerContext &C) const;

  ProgramStateRef evalAssume(ProgramStateRef State, SVal Cond,
                             bool Assumption) const;
};

void SAGenTestChecker::checkPostCall(const CallEvent &Call, CheckerContext &C) const {
  const Expr *OriginExpr = Call.getOriginExpr();
  const CallExpr *CE = dyn_cast_or_null<CallExpr>(OriginExpr);
  if (!CE || !ExprHasName(OriginExpr, "request_firmware", C) ||
      CE->getNumArgs() < 1)
    return;

  SymbolRef Status = Call.getReturnValue().getAsSymbol();
  if (!Status)
    return;

  const Expr *FirstArg = CE->getArg(0)->IgnoreParenImpCasts();
  const UnaryOperator *AddressOf = dyn_cast<UnaryOperator>(FirstArg);
  if (!AddressOf || AddressOf->getOpcode() != UO_AddrOf)
    return;

  const MemRegion *MR = getMemRegionFromExpr(CE->getArg(0), C);
  if (!MR)
    return;

  MR = MR->getBaseRegion();
  if (!MR)
    return;

  C.addTransition(C.getState()->set<RequestFwMap>(MR, Status));
}

void SAGenTestChecker::checkBranchCondition(const Stmt *Condition,
                                             CheckerContext &C) const {
  const Expr *Test = dyn_cast<Expr>(Condition);
  if (!Test)
    return;

  Test = Test->IgnoreParenImpCasts();
  if (const auto *Not = dyn_cast<UnaryOperator>(Test)) {
    if (Not->getOpcode() != UO_LNot)
      return;
    Test = Not->getSubExpr()->IgnoreParenImpCasts();
  }

  const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(Test);
  if (!DRE || !DRE->getType()->isPointerType())
    return;

  const MemRegion *MR = getMemRegionFromExpr(DRE, C);
  if (!MR)
    return;
  MR = MR->getBaseRegion();
  if (!MR)
    return;

  ProgramStateRef State = C.getState();
  if (!State->get<RequestFwMap>(MR))
    return;

  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT, "request_firmware() output tested before its status is proven successful", N);
  Report->addRange(Condition->getSourceRange());
  C.emitReport(std::move(Report));
  C.addTransition(State->remove<RequestFwMap>(MR));
}

ProgramStateRef SAGenTestChecker::evalAssume(ProgramStateRef State, SVal Cond,
                                             bool Assumption) const {
  ProgramStateRef NewState = State->assume(Cond, Assumption);
  if (!NewState)
    return nullptr;

  for (const auto &Entry : NewState->get<RequestFwMap>()) {
    const llvm::APSInt *Status =
        NewState->getStateManager().getConstraintManager().getSymVal(
            NewState, Entry.second);
    if (Status && Status->isZero())
      NewState = NewState->remove<RequestFwMap>(Entry.first);
  }
  return NewState;
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker", 
      "Detects when the firmware pointer returned by request_firmware() is directly checked instead of verifying its error code",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
