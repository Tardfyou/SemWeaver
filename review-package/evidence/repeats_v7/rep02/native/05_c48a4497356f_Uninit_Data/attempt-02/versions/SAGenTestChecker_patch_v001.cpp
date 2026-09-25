// /external/upstream-study/SAGEN/result-0224-bugfail-multi-o3mini/test-Uninit-Data-c48a4497356f701f94f1951626637ae240af909e/checkers/checker7.cpp
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Checkers/Taint.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/Environment.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramState.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramStateTrait.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ConstraintManager.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SymExpr.h"
#include "llvm/ADT/APSInt.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtVisitor.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/StaticAnalyzer/Checkers/utility.h"
#include "clang/Lex/Lexer.h"

using namespace clang;
using namespace ento;
using namespace taint;

// Map each request_firmware() output slot to the symbolic status returned by
// the same call until a path proves that status is zero.
REGISTER_MAP_WITH_PROGRAMSTATE(RequestFwMap, const MemRegion*, SymbolRef)

namespace {

class SAGenTestChecker : public Checker< check::PostCall, check::BranchCondition,
                                        eval::Assume > {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker() : BT(new BugType(this, "Unchecked request_firmware() output")) {}

  // Record the output slot and the status symbol produced by the same call.
  void checkPostCall(const CallEvent &Call, CheckerContext &C) const;

  // Report a use of a pending output in a branch condition.
  void checkBranchCondition(const Stmt *Condition, CheckerContext &C) const;

  // A pending output becomes initialized only on a path where its status is zero.
  ProgramStateRef evalAssume(ProgramStateRef State, SVal Cond,
                             bool Assumption) const;
};

void SAGenTestChecker::checkPostCall(const CallEvent &Call, CheckerContext &C) const {
  const Expr *OriginExpr = Call.getOriginExpr();
  const CallExpr *CE = dyn_cast_or_null<CallExpr>(OriginExpr);
  if (!CE || !ExprHasName(OriginExpr, "request_firmware", C) ||
      !CE->getType()->isIntegerType() || CE->getNumArgs() < 1)
    return;

  const Expr *FirstArg = CE->getArg(0)->IgnoreParenImpCasts();
  const UnaryOperator *AddressOf = dyn_cast<UnaryOperator>(FirstArg);
  if (!AddressOf || AddressOf->getOpcode() != UO_AddrOf ||
      !AddressOf->getSubExpr()->getType()->isPointerType())
    return;

  SymbolRef Status = Call.getReturnValue().getAsSymbol();
  if (!Status)
    return;

  // Use the original address expression: it denotes the caller's output slot.
  const MemRegion *MR = getMemRegionFromExpr(CE->getArg(0), C);
  if (!MR)
    return;

  MR = MR->getBaseRegion();
  if (!MR)
    return;

  C.addTransition(C.getState()->set<RequestFwMap>(MR, Status));
}

void SAGenTestChecker::checkBranchCondition(const Stmt *Condition, CheckerContext &C) const {
  ProgramStateRef State = C.getState();

  // Search downward in the condition expression for a DeclRefExpr.
  // This gives us the pointer variable being tested.
  const DeclRefExpr *DRE = findSpecificTypeInChildren<DeclRefExpr>(Condition);
  if (!DRE)
    return;

  // Obtain the memory region corresponding to the pointed variable.
  const MemRegion *MR = getMemRegionFromExpr(DRE, C);
  if (!MR)
    return;

  MR = MR->getBaseRegion();
  if (!MR)
    return;

  // A tracked slot is unsafe to read until its associated call status is known
  // to be zero on this path.
  if (State->get<RequestFwMap>(MR)) {
    ExplodedNode *N = C.generateNonFatalErrorNode();
    if (!N)
      return;

    auto Report = std::make_unique<PathSensitiveBugReport>(
        *BT, "request_firmware() output used before success is proven", N);
    Report->addRange(Condition->getSourceRange());
    C.emitReport(std::move(Report));
    State = State->remove<RequestFwMap>(MR);
  }
  C.addTransition(State);
}

ProgramStateRef SAGenTestChecker::evalAssume(ProgramStateRef State, SVal Cond,
                                             bool Assumption) const {
  const auto &Pending = State->get<RequestFwMap>();
  for (auto I = Pending.begin(), E = Pending.end(); I != E; ++I) {
    const llvm::APSInt *Status = State->getStateManager().getConstraintManager()
                                     .getSymVal(State, I->second);
    if (Status && Status->isZero())
      State = State->remove<RequestFwMap>(I->first);
  }
  return State;
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
