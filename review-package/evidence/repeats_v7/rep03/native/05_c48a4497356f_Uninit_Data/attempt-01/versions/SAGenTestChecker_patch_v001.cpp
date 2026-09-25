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

// Map each output slot from request_firmware() to that call's pending status.
REGISTER_MAP_WITH_PROGRAMSTATE(RequestFwMap, const MemRegion*, SymbolRef)

namespace {

class SAGenTestChecker : public Checker< check::PostCall, check::BranchCondition,
                                         eval::Assume > {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker() : BT(new BugType(this, "Unchecked return value of request_firmware()")) {}

  // Record the output slot and the status produced by the same fallible call.
  void checkPostCall(const CallEvent &Call, CheckerContext &C) const;

  // Report a direct read of an output whose producing status is still pending.
  void checkBranchCondition(const Stmt *Condition, CheckerContext &C) const;

  // A request_firmware() output is usable only after its status is proven zero.
  ProgramStateRef evalAssume(ProgramStateRef State, SVal Cond,
                             bool Assumption) const;
};

void SAGenTestChecker::checkPostCall(const CallEvent &Call, CheckerContext &C) const {
  const FunctionDecl *FD = dyn_cast_or_null<FunctionDecl>(Call.getDecl());
  if (!FD || FD->getName() != "request_firmware" || FD->getNumParams() < 1)
    return;

  // The contract is meaningful only for an address-taken pointer output.
  QualType OutputParamType = FD->getParamDecl(0)->getType();
  if (!OutputParamType->isPointerType() ||
      !OutputParamType->getPointeeType()->isPointerType())
    return;

  const Expr *OriginExpr = Call.getOriginExpr();
  const CallExpr *CE = dyn_cast_or_null<CallExpr>(OriginExpr);
  if (!CE || CE->getNumArgs() < 1)
    return;

  const Expr *OutputAddress = CE->getArg(0)->IgnoreParenImpCasts();
  const UnaryOperator *AddressOf = dyn_cast<UnaryOperator>(OutputAddress);
  if (!AddressOf || AddressOf->getOpcode() != UO_AddrOf)
    return;

  const MemRegion *MR = getMemRegionFromExpr(CE->getArg(0), C);
  if (!MR)
    return;
  MR = MR->getBaseRegion();
  if (!MR)
    return;

  SymbolRef Status = Call.getReturnValue().getAsSymbol();
  if (!Status)
    return;

  C.addTransition(C.getState()->set<RequestFwMap>(MR, Status));
}

ProgramStateRef SAGenTestChecker::evalAssume(ProgramStateRef State, SVal Cond,
                                             bool Assumption) const {
  (void)Cond;
  (void)Assumption;

  // The constraint manager has incorporated this branch assumption already.
  // Remove only outputs whose associated status is concretely the success value.
  auto Pending = State->get<RequestFwMap>();
  for (auto I = Pending.begin(), E = Pending.end(); I != E; ++I) {
    const llvm::APSInt *Value =
        State->getConstraintManager().getSymVal(State, I->second);
    if (Value && Value->isZero())
      State = State->remove<RequestFwMap>(I->first);
  }
  return State;
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

  // A direct read is unsafe while the producing call's status is still pending.
  const SymbolRef *PendingStatus = State->get<RequestFwMap>(MR);
  if (PendingStatus) {
    // Create a non-fatal error node to report the bug.
    ExplodedNode *N = C.generateNonFatalErrorNode();
    if (!N)
      return;

    auto Report = std::make_unique<PathSensitiveBugReport>(
        *BT, "Unchecked return value of request_firmware(): firmware pointer used in condition", N);
    Report->addRange(Condition->getSourceRange());
    C.emitReport(std::move(Report));

    // Optionally, one might clear the entry to avoid duplicate reports.
    State = State->remove<RequestFwMap>(MR);
  }
  C.addTransition(State);
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
