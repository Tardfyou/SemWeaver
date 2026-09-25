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

// Store each request_firmware() output slot with the status symbol returned by
// the same call until that status is constrained to the success value.
REGISTER_MAP_WITH_PROGRAMSTATE(RequestFwMap, const MemRegion*, SymbolRef)

namespace {

class SAGenTestChecker : public Checker< check::PostCall, check::Location,
                                        eval::Assume > {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker() : BT(new BugType(this, "Unchecked return value of request_firmware()")) {}

  void checkPostCall(const CallEvent &Call, CheckerContext &C) const;
  void checkLocation(SVal Loc, bool IsLoad, const Stmt *S,
                     CheckerContext &C) const;
  ProgramStateRef evalAssume(ProgramStateRef State, SVal Cond,
                             bool Assumption) const;
};

void SAGenTestChecker::checkPostCall(const CallEvent &Call, CheckerContext &C) const {
  const Expr *OriginExpr = Call.getOriginExpr();
  const CallExpr *CE = dyn_cast_or_null<CallExpr>(OriginExpr);
  if (!CE || !ExprHasName(OriginExpr, "request_firmware", C))
    return;

  const FunctionDecl *Callee = Call.getDecl();
  if (!Callee || CE->getNumArgs() < 1 || Callee->getNumParams() < 1)
    return;

  QualType OutputParamType = Callee->getParamDecl(0)->getType();
  if (!OutputParamType->isPointerType() ||
      !OutputParamType->getPointeeType()->isPointerType())
    return;

  const Expr *FirstArg = CE->getArg(0);
  const UnaryOperator *AddressOf =
      dyn_cast<UnaryOperator>(FirstArg->IgnoreParenImpCasts());
  if (!AddressOf || AddressOf->getOpcode() != UO_AddrOf)
    return;

  const MemRegion *MR = getMemRegionFromExpr(FirstArg, C);
  auto Status = Call.getReturnValue().getAsSymbol();
  if (!MR || !Status)
    return;

  MR = MR->getBaseRegion();
  if (!MR)
    return;

  C.addTransition(C.getState()->set<RequestFwMap>(MR, *Status));
}

void SAGenTestChecker::checkLocation(SVal Loc, bool IsLoad, const Stmt *S,
                                     CheckerContext &C) const {
  if (!IsLoad)
    return;

  const MemRegion *MR = Loc.getAsRegion();
  if (!MR)
    return;

  MR = MR->getBaseRegion();
  if (!MR || !C.getState()->get<RequestFwMap>(MR))
    return;

  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT, "request_firmware() output used before its return status is proven successful", N);
  if (S)
    Report->addRange(S->getSourceRange());
  C.emitReport(std::move(Report));
  C.addTransition(C.getState()->remove<RequestFwMap>(MR));
}

ProgramStateRef SAGenTestChecker::evalAssume(ProgramStateRef State, SVal Cond,
                                             bool Assumption) const {
  for (const auto &Entry : State->get<RequestFwMap>()) {
    const llvm::APSInt *Status =
        State->getConstraintManager().getSymVal(State, Entry.second);
    if (Status && Status->isZero())
      State = State->remove<RequestFwMap>(Entry.first);
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
