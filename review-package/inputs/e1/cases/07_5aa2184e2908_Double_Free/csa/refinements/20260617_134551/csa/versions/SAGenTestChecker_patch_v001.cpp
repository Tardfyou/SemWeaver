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

// Track calls whose return status controls whether an object-owned resource was established.
REGISTER_MAP_WITH_PROGRAMSTATE(CallResultOwnerMap, const MemRegion *, const MemRegion *)

namespace {

class SAGenTestChecker
    : public Checker<check::Bind, check::PreStmt<GotoStmt>> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker() : BT(new BugType(this, "Incorrect free in error path")) {}

  void checkBind(SVal Loc, SVal Val, const Stmt *S, CheckerContext &C) const;
  void checkPreStmt(const GotoStmt *GS, CheckerContext &C) const;

private:
  const MemRegion *getBaseRegionFromExpr(const Expr *E, CheckerContext &C) const;
  const MemRegion *getGuardRegion(const GotoStmt *GS, CheckerContext &C) const;
  const MemberExpr *getFreedMember(const LabelStmt *LS, CheckerContext &C) const;
  void reportPrematureCleanup(const GotoStmt *GS, CheckerContext &C) const;
};

const MemRegion *SAGenTestChecker::getBaseRegionFromExpr(const Expr *E,
                                                         CheckerContext &C) const {
  if (!E)
    return nullptr;

  const MemRegion *MR = getMemRegionFromExpr(E->IgnoreParenImpCasts(), C);
  if (!MR)
    return nullptr;
  return MR->getBaseRegion();
}

void SAGenTestChecker::checkBind(SVal Loc, SVal Val, const Stmt *S,
                                 CheckerContext &C) const {
  if (!S)
    return;

  const CallExpr *CE = dyn_cast<CallExpr>(S);
  if (!CE)
    CE = findSpecificTypeInChildren<CallExpr>(S);
  if (!CE)
    return;

  const MemRegion *ResultReg = Loc.getAsRegion();
  if (!ResultReg)
    return;
  ResultReg = ResultReg->getBaseRegion();
  if (!ResultReg)
    return;

  ProgramStateRef State = C.getState();
  bool Changed = false;
  for (const Expr *Arg : CE->arguments()) {
    const Expr *StrippedArg = Arg->IgnoreParenImpCasts();
    QualType ArgTy = StrippedArg->getType();
    if (!ArgTy->isPointerType())
      continue;

    QualType PointeeTy = ArgTy->getPointeeType();
    if (!PointeeTy->isStructureOrClassType())
      continue;

    const MemRegion *OwnerReg = getBaseRegionFromExpr(StrippedArg, C);
    if (!OwnerReg)
      continue;

    State = State->set<CallResultOwnerMap>(OwnerReg, ResultReg);
    Changed = true;
  }

  if (Changed)
    C.addTransition(State);
}

const MemRegion *SAGenTestChecker::getGuardRegion(const GotoStmt *GS,
                                                  CheckerContext &C) const {
  const IfStmt *IS = findSpecificTypeInParents<IfStmt>(GS, C);
  if (!IS)
    return nullptr;

  return getBaseRegionFromExpr(IS->getCond(), C);
}

const MemberExpr *SAGenTestChecker::getFreedMember(const LabelStmt *LS,
                                                   CheckerContext &C) const {
  if (!LS)
    return nullptr;

  const CallExpr *CE = dyn_cast<CallExpr>(LS->getSubStmt());
  if (!CE)
    CE = findSpecificTypeInChildren<CallExpr>(LS->getSubStmt());
  if (!CE || !ExprHasName(CE, "kfree", C) || CE->getNumArgs() == 0)
    return nullptr;

  return findSpecificTypeInChildren<MemberExpr>(CE->getArg(0));
}

void SAGenTestChecker::checkPreStmt(const GotoStmt *GS, CheckerContext &C) const {
  const LabelStmt *Target = GS->getLabel()->getStmt();
  const MemberExpr *FreedMember = getFreedMember(Target, C);
  if (!FreedMember)
    return;

  const MemRegion *OwnerReg = getBaseRegionFromExpr(FreedMember->getBase(), C);
  const MemRegion *GuardReg = getGuardRegion(GS, C);
  if (!OwnerReg || !GuardReg)
    return;

  ProgramStateRef State = C.getState();
  const MemRegion *const *TrackedGuard = State->get<CallResultOwnerMap>(OwnerReg);
  if (!TrackedGuard || *TrackedGuard != GuardReg)
    return;

  reportPrematureCleanup(GS, C);
}

void SAGenTestChecker::reportPrematureCleanup(const GotoStmt *GS,
                                              CheckerContext &C) const {
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT, "Error path frees an object-owned resource before the guarded call has established ownership", N);
  Report->addRange(GS->getSourceRange());
  C.emitReport(std::move(Report));
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
