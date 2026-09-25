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

// Associate each request_firmware() output slot with the status returned by
// the same call until the status-zero success path has been established.
REGISTER_MAP_WITH_PROGRAMSTATE(RequestFwMap, const MemRegion*, SymbolRef)

namespace {

class SAGenTestChecker : public Checker< check::PostCall, check::BranchCondition, eval::Assume > {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker() : BT(new BugType(this, "Unchecked return value of request_firmware()")) {}

  // Callback to record the firmware pointer coming from the request_firmware() call.
  void checkPostCall(const CallEvent &Call, CheckerContext &C) const;

  // Callback to detect use of a firmware output before its status is proven successful.
  void checkBranchCondition(const Stmt *Condition, CheckerContext &C) const;

  // Clear outputs only on paths where their matching request status is zero.
  ProgramStateRef evalAssume(ProgramStateRef State, SVal Cond,
                             bool Assumption) const;

private:
  // Helper function can be added here if needed.
};

void SAGenTestChecker::checkPostCall(const CallEvent &Call, CheckerContext &C) const {
  ProgramStateRef State = C.getState();

  // Check that the callee is "request_firmware" by using the origin expression.
  const Expr *OriginExpr = Call.getOriginExpr();
  if (!OriginExpr)
    return;

  // Verify we are handling a call expression.
  const CallExpr *CE = dyn_cast<CallExpr>(OriginExpr);
  if (!CE)
    return;

  // Check the callee's name using the origin expression.
  // Instead of using Call.getCalleeIdentifier() (which points to "request_firmware")
  // we use the utility function to be more accurate if the source text has been modified.
  if (!ExprHasName(OriginExpr, "request_firmware", C))
    return;

  // request_firmware() reports success through its integer return value and
  // initializes its first, address-taken output only on that success path.
  if (CE->getNumArgs() < 1)
    return;

  const Expr *FirstArg = CE->getArg(0);
  const auto *OutputAddress = FirstArg
      ? dyn_cast<UnaryOperator>(FirstArg->IgnoreParenImpCasts())
      : nullptr;
  if (!OutputAddress || OutputAddress->getOpcode() != UO_AddrOf)
    return;

  const FunctionDecl *Callee = CE->getDirectCallee();
  if (!Callee || Callee->getNumParams() < 1 ||
      !Callee->getParamDecl(0)->getType()->isPointerType())
    return;

  SymbolRef Status = Call.getReturnValue().getAsSymbol();
  if (!Status)
    return;

  // Obtain the output-slot region from the original address expression.
  const MemRegion *MR = getMemRegionFromExpr(FirstArg, C);
  if (!MR)
    return;

  MR = MR->getBaseRegion();
  if (!MR)
    return;

  // Keep the matching status until a path proves that it is the zero-success
  // result required by request_firmware()'s output contract.
  State = State->set<RequestFwMap>(MR, Status);
  C.addTransition(State);
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

  // A read of this output is unsafe until the matching status has been proven
  // zero on the current path.
  const SymbolRef *Status = State->get<RequestFwMap>(MR);
  if (Status) {
    // Create a non-fatal error node to report the bug.
    ExplodedNode *N = C.generateNonFatalErrorNode();
    if (!N)
      return;

    auto Report = std::make_unique<PathSensitiveBugReport>(
        *BT, "Unchecked return value of request_firmware(): firmware pointer used in condition", N);
    Report->addRange(Condition->getSourceRange());
    C.emitReport(std::move(Report));

    // Avoid duplicate reports for the same unchecked output on this path.
    State = State->remove<RequestFwMap>(MR);
  }
  C.addTransition(State);
}

ProgramStateRef SAGenTestChecker::evalAssume(ProgramStateRef State, SVal Cond,
                                             bool Assumption) const {
  SymbolRef Status = Cond.getAsSymbol();
  if (!Status)
    return State;

  const llvm::APSInt *Value =
      State->getConstraintManager().getSymVal(State, Status);
  if (!Value || !Value->isZero())
    return State;

  auto Outputs = State->get<RequestFwMap>();
  for (auto I = Outputs.begin(), E = Outputs.end(); I != E; ++I) {
    if (I->second == Status)
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
