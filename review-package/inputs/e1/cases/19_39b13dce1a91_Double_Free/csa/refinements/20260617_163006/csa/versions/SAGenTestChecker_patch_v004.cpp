// /external/upstream-study/SAGEN/result-0224-bugfail-multi-o3mini/test-Double-Free-39b13dce1a91cdfc3bec9238f9e89094551bd428/checkers/checker0.cpp
#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "clang/StaticAnalyzer/Checkers/utility.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"

using namespace clang;
using namespace ento;

namespace {

struct DevmActionRegistration {
  const CallExpr *Call = nullptr;
  const FunctionDecl *CleanupFn = nullptr;
  const Expr *DataExpr = nullptr;
};

const Expr *ignoreCastsAndAddressOf(const Expr *E) {
  if (!E)
    return nullptr;

  E = E->IgnoreParenImpCasts();
  if (const auto *UO = dyn_cast<UnaryOperator>(E)) {
    if (UO->getOpcode() == UO_AddrOf)
      return ignoreCastsAndAddressOf(UO->getSubExpr());
  }
  return E;
}

const FunctionDecl *getReferencedFunction(const Expr *E) {
  E = ignoreCastsAndAddressOf(E);
  if (!E)
    return nullptr;

  if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
    return dyn_cast<FunctionDecl>(DRE->getDecl());
  return nullptr;
}

bool stmtContains(const Stmt *Root, const Stmt *Needle) {
  if (!Root || !Needle)
    return false;
  if (Root == Needle)
    return true;

  for (const Stmt *Child : Root->children()) {
    if (stmtContains(Child, Needle))
      return true;
  }
  return false;
}

const CallExpr *findDevmAddActionOrResetCall(const Stmt *S) {
  if (!S)
    return nullptr;

  if (const auto *CE = dyn_cast<CallExpr>(S)) {
    if (const FunctionDecl *Callee = CE->getDirectCallee()) {
      if (Callee->getName() == "devm_add_action_or_reset")
        return CE;
    }
  }

  for (const Stmt *Child : S->children()) {
    if (const CallExpr *Found = findDevmAddActionOrResetCall(Child))
      return Found;
  }
  return nullptr;
}

bool exprIsZero(const Expr *E) {
  E = E ? E->IgnoreParenImpCasts() : nullptr;
  if (const auto *IL = dyn_cast_or_null<IntegerLiteral>(E))
    return IL->getValue().isZero();
  return false;
}

bool conditionFailureBranchIsThen(const Expr *Cond, const CallExpr *ActionCall) {
  Cond = Cond ? Cond->IgnoreParenImpCasts() : nullptr;
  if (!Cond || !ActionCall)
    return true;

  if (const auto *UO = dyn_cast<UnaryOperator>(Cond)) {
    if (UO->getOpcode() == UO_LNot && stmtContains(UO->getSubExpr(), ActionCall))
      return false;
  }

  if (const auto *BO = dyn_cast<BinaryOperator>(Cond)) {
    const bool CallOnLHS = stmtContains(BO->getLHS(), ActionCall);
    const bool CallOnRHS = stmtContains(BO->getRHS(), ActionCall);
    const bool OtherIsZero = (CallOnLHS && exprIsZero(BO->getRHS())) ||
                             (CallOnRHS && exprIsZero(BO->getLHS()));
    if (OtherIsZero && BO->getOpcode() == BO_EQ)
      return false;
    if (OtherIsZero && BO->getOpcode() == BO_NE)
      return true;
  }

  return true;
}

bool sameFunction(const FunctionDecl *LHS, const FunctionDecl *RHS) {
  if (!LHS || !RHS)
    return false;
  return LHS->getCanonicalDecl() == RHS->getCanonicalDecl();
}

bool sameCleanupData(const Expr *RegisteredData, const Expr *ExplicitData,
                     CheckerContext &C) {
  const MemRegion *RegisteredRegion = getMemRegionFromExpr(RegisteredData, C);
  const MemRegion *ExplicitRegion = getMemRegionFromExpr(ExplicitData, C);
  return RegisteredRegion && ExplicitRegion && RegisteredRegion == ExplicitRegion;
}

class SAGenTestChecker : public Checker<check::PreCall> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Redundant devm cleanup", "Double Free")) {}

  void checkPreCall(const CallEvent &Call, CheckerContext &C) const;

private:
  void reportRedundantCleanup(const CallEvent &Call, CheckerContext &C) const;
};

void SAGenTestChecker::checkPreCall(const CallEvent &Call,
                                    CheckerContext &C) const {
  const Expr *OriginExpr = Call.getOriginExpr();
  const auto *ExplicitCleanup = dyn_cast_or_null<FunctionDecl>(Call.getDecl());
  if (!OriginExpr || !ExplicitCleanup || Call.getNumArgs() < 1)
    return;

  const IfStmt *ParentIf = findSpecificTypeInParents<IfStmt>(OriginExpr, C);
  if (!ParentIf || !ParentIf->getCond())
    return;

  const bool InThen = stmtContains(ParentIf->getThen(), OriginExpr);
  const bool InElse = stmtContains(ParentIf->getElse(), OriginExpr);
  if (!InThen && !InElse)
    return;

  const CallExpr *RegistrationCall =
      findDevmAddActionOrResetCall(ParentIf->getCond());
  if (!RegistrationCall || RegistrationCall->getNumArgs() < 3)
    return;

  const bool FailureBranchIsThen =
      conditionFailureBranchIsThen(ParentIf->getCond(), RegistrationCall);
  if ((InThen && !FailureBranchIsThen) || (InElse && FailureBranchIsThen))
    return;

  const FunctionDecl *RegisteredCleanup =
      getReferencedFunction(RegistrationCall->getArg(1));
  if (!sameFunction(RegisteredCleanup, ExplicitCleanup))
    return;

  if (!sameCleanupData(RegistrationCall->getArg(2), Call.getArgExpr(0), C))
    return;

  reportRedundantCleanup(Call, C);
}

void SAGenTestChecker::reportRedundantCleanup(const CallEvent &Call,
                                              CheckerContext &C) const {
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT, "cleanup registered with devm_add_action_or_reset is called again on its failure path", N);
  Report->addRange(Call.getSourceRange());
  C.emitReport(std::move(Report));
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker", 
      "Detects redundant cleanup call in error handling leading to double free", "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
