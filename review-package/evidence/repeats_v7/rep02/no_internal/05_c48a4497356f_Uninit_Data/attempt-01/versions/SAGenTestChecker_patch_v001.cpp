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

// Track each request_firmware() output slot with the status symbol returned by
// the same call until a path proves that status is zero.
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

private:
  // Helper function can be added here if needed.
};

void SAGenTestChecker::checkPostCall(const CallEvent &Call, CheckerContext &C) const {
  ProgramStateRef State = C.getState();

  const Expr *OriginExpr = Call.getOriginExpr();
  const CallExpr *CE = dyn_cast_or_null<CallExpr>(OriginExpr);
  if (!CE || !ExprHasName(OriginExpr, "request_firmware", C))
    return;

  const FunctionDecl *FD = CE->getDirectCallee();
  if (!FD || CE->getNumArgs() < 1 || FD->getNumParams() < 1)
    return;

  const QualType OutputParamType = FD->getParamDecl(0)->getType();
  if (!OutputParamType->isPointerType() ||
      !OutputParamType->getPointeeType()->isPointerType())
    return;

  const Expr *FirstArg = CE->getArg(0);
  const UnaryOperator *AddressOf =
      dyn_cast<UnaryOperator>(FirstArg->IgnoreParenImpCasts());
  if (!AddressOf || AddressOf->getOpcode() != UO_AddrOf ||
      !AddressOf->getSubExpr()->getType()->isPointerType())
    return;

  SymbolRef Status = Call.getReturnValue().getAsSymbol();
  if (!Status)
    return;

  // Use the address expression itself so the output slot, rather than its
  // uninitialized pointer value, identifies the tracked region.
  const MemRegion *MR = getMemRegionFromExpr(FirstArg, C);
  if (!MR)
    return;

  MR = MR->getBaseRegion();
  if (!MR)
    return;

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

  // The output is unsafe to inspect while the corresponding request status has
  // not been proven successful on this path.
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

    // Avoid duplicate reports for the same pending output on this path.
    State = State->remove<RequestFwMap>(MR);
  }
  C.addTransition(State);
}

ProgramStateRef SAGenTestChecker::evalAssume(ProgramStateRef State, SVal,
                                             bool) const {
  const auto Pending = State->get<RequestFwMap>();
  for (auto I = Pending.begin(), E = Pending.end(); I != E; ++I) {
    const llvm::APSInt *Value =
        State->getConstraintManager().getSymVal(State, I->second);
    if (Value && Value->isZero())
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
