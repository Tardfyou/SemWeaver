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

// request_firmware() initializes its out parameter only when the returned status
// is checked successfully.  Track the out parameter until the status variable is
// used as a branch barrier; using the out parameter before that is unsafe.
REGISTER_MAP_WITH_PROGRAMSTATE(RequestFwMap, const MemRegion*, bool)
REGISTER_MAP_WITH_PROGRAMSTATE(RequestFwRetMap, const MemRegion*, const MemRegion*)

namespace {

class RegionUseVisitor : public RecursiveASTVisitor<RegionUseVisitor> {
  CheckerContext &C;
  const MemRegion *Target;
  bool Found = false;

public:
  RegionUseVisitor(CheckerContext &C, const MemRegion *Target)
      : C(C), Target(Target) {}

  bool VisitDeclRefExpr(DeclRefExpr *DRE) {
    const MemRegion *MR = getMemRegionFromExpr(DRE, C);
    if (MR && MR->getBaseRegion() == Target)
      Found = true;
    return !Found;
  }

  bool found() const { return Found; }
};

class ReturnGuardVisitor : public RecursiveASTVisitor<ReturnGuardVisitor> {
  CheckerContext &C;
  ProgramStateRef State;
  const MemRegion *GuardRegion = nullptr;
  const MemRegion *FirmwareRegion = nullptr;

public:
  ReturnGuardVisitor(CheckerContext &C, ProgramStateRef State)
      : C(C), State(State) {}

  bool VisitDeclRefExpr(DeclRefExpr *DRE) {
    const MemRegion *MR = getMemRegionFromExpr(DRE, C);
    if (!MR)
      return true;

    MR = MR->getBaseRegion();
    if (const MemRegion *const *TrackedFw = State->get<RequestFwRetMap>(MR)) {
      GuardRegion = MR;
      FirmwareRegion = *TrackedFw;
      return false;
    }
    return true;
  }

  const MemRegion *getGuardRegion() const { return GuardRegion; }
  const MemRegion *getFirmwareRegion() const { return FirmwareRegion; }
};

class SAGenTestChecker : public Checker< check::PostCall, check::PreCall, check::BranchCondition > {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker() : BT(new BugType(this, "Unchecked return value of request_firmware()")) {}

  void checkPostCall(const CallEvent &Call, CheckerContext &C) const;
  void checkPreCall(const CallEvent &Call, CheckerContext &C) const;
  void checkBranchCondition(const Stmt *Condition, CheckerContext &C) const;

private:
  const MemRegion *getBaseRegionFromExpr(const Expr *E, CheckerContext &C) const;
  const MemRegion *getAssignedReturnRegion(const CallExpr *CE, CheckerContext &C) const;
  bool conditionUsesRegion(const Stmt *Condition, const MemRegion *MR, CheckerContext &C) const;
  void reportUncheckedUse(const Stmt *S, const char *Message, CheckerContext &C) const;
};

const MemRegion *SAGenTestChecker::getBaseRegionFromExpr(const Expr *E, CheckerContext &C) const {
  if (!E)
    return nullptr;

  E = E->IgnoreParenImpCasts();
  if (const auto *UO = dyn_cast<UnaryOperator>(E)) {
    if (UO->getOpcode() == UO_AddrOf)
      E = UO->getSubExpr()->IgnoreParenImpCasts();
  }

  const MemRegion *MR = getMemRegionFromExpr(E, C);
  return MR ? MR->getBaseRegion() : nullptr;
}

const MemRegion *SAGenTestChecker::getAssignedReturnRegion(const CallExpr *CE, CheckerContext &C) const {
  const auto *BO = findSpecificTypeInParents<BinaryOperator>(CE, C);
  if (!BO || !BO->isAssignmentOp())
    return nullptr;

  if (BO->getRHS()->IgnoreParenImpCasts() != CE)
    return nullptr;

  return getBaseRegionFromExpr(BO->getLHS(), C);
}

bool SAGenTestChecker::conditionUsesRegion(const Stmt *Condition, const MemRegion *MR, CheckerContext &C) const {
  if (!Condition || !MR)
    return false;

  RegionUseVisitor Visitor(C, MR);
  Visitor.TraverseStmt(const_cast<Stmt *>(Condition));
  return Visitor.found();
}

void SAGenTestChecker::reportUncheckedUse(const Stmt *S, const char *Message, CheckerContext &C) const {
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto Report = std::make_unique<PathSensitiveBugReport>(*BT, Message, N);
  Report->addRange(S->getSourceRange());
  C.emitReport(std::move(Report));
}

void SAGenTestChecker::checkPostCall(const CallEvent &Call, CheckerContext &C) const {
  const Expr *OriginExpr = Call.getOriginExpr();
  const auto *CE = dyn_cast_or_null<CallExpr>(OriginExpr);
  if (!CE || !ExprHasName(OriginExpr, "request_firmware", C) || CE->getNumArgs() < 1)
    return;

  const MemRegion *FirmwareRegion = getBaseRegionFromExpr(CE->getArg(0), C);
  if (!FirmwareRegion)
    return;

  ProgramStateRef State = C.getState()->set<RequestFwMap>(FirmwareRegion, true);
  if (const MemRegion *RetRegion = getAssignedReturnRegion(CE, C))
    State = State->set<RequestFwRetMap>(RetRegion, FirmwareRegion);
  C.addTransition(State);
}

void SAGenTestChecker::checkPreCall(const CallEvent &Call, CheckerContext &C) const {
  const Expr *OriginExpr = Call.getOriginExpr();
  const auto *CE = dyn_cast_or_null<CallExpr>(OriginExpr);
  if (!CE || !ExprHasName(OriginExpr, "release_firmware", C) || CE->getNumArgs() < 1)
    return;

  const MemRegion *FirmwareRegion = getBaseRegionFromExpr(CE->getArg(0), C);
  ProgramStateRef State = C.getState();
  const bool *Unverified = FirmwareRegion ? State->get<RequestFwMap>(FirmwareRegion) : nullptr;
  if (!Unverified || !*Unverified)
    return;

  reportUncheckedUse(OriginExpr, "release_firmware() called on firmware data before checking request_firmware() status", C);
  C.addTransition(State->remove<RequestFwMap>(FirmwareRegion));
}

void SAGenTestChecker::checkBranchCondition(const Stmt *Condition, CheckerContext &C) const {
  ProgramStateRef State = C.getState();

  ReturnGuardVisitor GuardVisitor(C, State);
  GuardVisitor.TraverseStmt(const_cast<Stmt *>(Condition));
  if (const MemRegion *GuardRegion = GuardVisitor.getGuardRegion()) {
    if (const MemRegion *FirmwareRegion = GuardVisitor.getFirmwareRegion())
      State = State->remove<RequestFwMap>(FirmwareRegion);
    State = State->remove<RequestFwRetMap>(GuardRegion);
    C.addTransition(State);
    return;
  }

  const DeclRefExpr *DRE = findSpecificTypeInChildren<DeclRefExpr>(Condition);
  if (!DRE)
    return;

  const MemRegion *FirmwareRegion = getBaseRegionFromExpr(DRE, C);
  const bool *Unverified = FirmwareRegion ? State->get<RequestFwMap>(FirmwareRegion) : nullptr;
  if (!Unverified || !*Unverified || !conditionUsesRegion(Condition, FirmwareRegion, C))
    return;

  reportUncheckedUse(Condition, "firmware data used before checking request_firmware() status", C);
  C.addTransition(State->remove<RequestFwMap>(FirmwareRegion));
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
