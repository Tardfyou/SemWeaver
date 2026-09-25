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
  bool exprReferencesDecl(const Expr *E, const ValueDecl *VD) const;
  bool containsPointerProducingCall(const Expr *E) const;
  bool stmtAssignsPointerCallToParamMember(const Stmt *S,
                                           const ValueDecl *Param) const;
  bool calleeEstablishesOwnedMemberForArg(const CallExpr *CE,
                                          unsigned ArgIdx) const;
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

bool SAGenTestChecker::exprReferencesDecl(const Expr *E,
                                          const ValueDecl *VD) const {
  if (!E || !VD)
    return false;

  E = E->IgnoreParenImpCasts();
  if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
    return DRE->getDecl() == VD;
  if (const auto *ME = dyn_cast<MemberExpr>(E))
    return exprReferencesDecl(ME->getBase(), VD);
  if (const auto *UO = dyn_cast<UnaryOperator>(E))
    return UO->getOpcode() == UO_Deref && exprReferencesDecl(UO->getSubExpr(), VD);

  for (const Stmt *Child : E->children())
    if (const auto *ChildExpr = dyn_cast_or_null<Expr>(Child))
      if (exprReferencesDecl(ChildExpr, VD))
        return true;
  return false;
}

bool SAGenTestChecker::containsPointerProducingCall(const Expr *E) const {
  if (!E)
    return false;

  E = E->IgnoreParenImpCasts();
  if (const auto *CE = dyn_cast<CallExpr>(E))
    return CE->getType()->isPointerType();
  if (isa<CXXNewExpr>(E))
    return true;

  for (const Stmt *Child : E->children())
    if (const auto *ChildExpr = dyn_cast_or_null<Expr>(Child))
      if (containsPointerProducingCall(ChildExpr))
        return true;
  return false;
}

bool SAGenTestChecker::stmtAssignsPointerCallToParamMember(
    const Stmt *S, const ValueDecl *Param) const {
  if (!S || !Param)
    return false;

  if (const auto *BO = dyn_cast<BinaryOperator>(S)) {
    if (BO->isAssignmentOp()) {
      const auto *ME = dyn_cast<MemberExpr>(BO->getLHS()->IgnoreParenImpCasts());
      if (ME && exprReferencesDecl(ME->getBase(), Param) &&
          containsPointerProducingCall(BO->getRHS()))
        return true;
    }
  }

  for (const Stmt *Child : S->children())
    if (stmtAssignsPointerCallToParamMember(Child, Param))
      return true;
  return false;
}

bool SAGenTestChecker::calleeEstablishesOwnedMemberForArg(
    const CallExpr *CE, unsigned ArgIdx) const {
  if (!CE)
    return false;

  const FunctionDecl *FD = CE->getDirectCallee();
  if (!FD || ArgIdx >= FD->getNumParams())
    return false;

  const Stmt *Body = FD->getBody();
  if (!Body)
    return false;

  return stmtAssignsPointerCallToParamMember(Body, FD->getParamDecl(ArgIdx));
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
  for (unsigned I = 0, E = CE->getNumArgs(); I != E; ++I) {
    const Expr *Arg = CE->getArg(I);
    const Expr *StrippedArg = Arg->IgnoreParenImpCasts();
    QualType ArgTy = StrippedArg->getType();
    if (!ArgTy->isPointerType())
      continue;

    QualType PointeeTy = ArgTy->getPointeeType();
    if (!PointeeTy->isStructureOrClassType())
      continue;

    if (!calleeEstablishesOwnedMemberForArg(CE, I))
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
