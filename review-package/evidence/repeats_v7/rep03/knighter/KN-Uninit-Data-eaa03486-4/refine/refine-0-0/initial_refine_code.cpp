#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Checkers/Taint.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/Environment.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/MemRegion.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramState.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramStateTrait.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SymExpr.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtVisitor.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/StaticAnalyzer/Checkers/utility.h"

#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"

#include <memory>

using namespace clang;
using namespace ento;
using namespace taint;

// VarDecl* -> bool. false means the local variable has not been initialized
// on the current path; true means a definite non-undefined write was observed.
REGISTER_MAP_WITH_PROGRAMSTATE(UninitVarMap, const VarDecl *, bool)

namespace {

class SAGenTestChecker
    : public Checker<check::PostStmt<DeclStmt>, check::Bind,
                     check::PreStmt<ReturnStmt>> {
  mutable std::unique_ptr<BugType> BT;

  static bool isTrackedRet(const VarDecl *VD);
  static const VarDecl *getBoundVarDecl(SVal Loc);

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Uninitialized Variable",
                       "Uninitialized ret usage")) {}

  void checkPostStmt(const DeclStmt *DS, CheckerContext &C) const;
  void checkBind(SVal Loc, SVal Val, const Stmt *S,
                 CheckerContext &C) const;
  void checkPreStmt(const ReturnStmt *RS, CheckerContext &C) const;
};

bool SAGenTestChecker::isTrackedRet(const VarDecl *VD) {
  if (!VD || !VD->getIdentifier() || VD->getName() != "ret")
    return false;

  // Static locals and global variables have language-defined zero
  // initialization. Only automatic local variables can have this defect.
  return VD->isLocalVarDecl() && !VD->hasGlobalStorage();
}

const VarDecl *SAGenTestChecker::getBoundVarDecl(SVal Loc) {
  const MemRegion *Region = Loc.getAsRegion();
  if (!Region)
    return nullptr;

  // Stores may target a subregion. The base region identifies the owning
  // local variable for direct stores and analyzer-resolved alias stores.
  Region = Region->getBaseRegion();

  const auto *VR = dyn_cast<VarRegion>(Region);
  if (!VR)
    return nullptr;

  return VR->getDecl();
}

// Record automatic local variables named "ret" that lack an initializer.
void SAGenTestChecker::checkPostStmt(const DeclStmt *DS,
                                     CheckerContext &C) const {
  ProgramStateRef State = C.getState();

  for (const Decl *D : DS->decls()) {
    const auto *VD = dyn_cast<VarDecl>(D);
    if (!isTrackedRet(VD) || VD->hasInit())
      continue;

    State = State->set<UninitVarMap>(VD, false);
  }

  if (State != C.getState())
    C.addTransition(State);
}

// Update state from the actual destination location, rather than from S.
// For "ret = function_call()", S is generally the BinaryOperator or call
// expression, not the DeclRefExpr for the left-hand side.
void SAGenTestChecker::checkBind(SVal Loc, SVal Val, const Stmt *S,
                                 CheckerContext &C) const {
  const VarDecl *VD = getBoundVarDecl(Loc);
  if (!isTrackedRet(VD))
    return;

  ProgramStateRef State = C.getState();
  const bool *Initialized = State->get<UninitVarMap>(VD);

  // Do not start tracking variables that were initialized at declaration,
  // and do not consider propagation of an undefined value an initialization.
  if (!Initialized || *Initialized || Val.isUndef())
    return;

  State = State->set<UninitVarMap>(VD, true);
  C.addTransition(State);
}

void SAGenTestChecker::checkPreStmt(const ReturnStmt *RS,
                                    CheckerContext &C) const {
  const Expr *RetExpr = RS->getRetValue();
  if (!RetExpr)
    return;

  RetExpr = RetExpr->IgnoreParenImpCasts();

  const auto *DRE = dyn_cast<DeclRefExpr>(RetExpr);
  if (!DRE)
    return;

  const auto *VD = dyn_cast<VarDecl>(DRE->getDecl());
  if (!isTrackedRet(VD))
    return;

  ProgramStateRef State = C.getState();
  const bool *Initialized = State->get<UninitVarMap>(VD);

  if (!Initialized || *Initialized)
    return;

  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT, "Uninitialized variable 'ret' used", N);
  Report->addRange(RetExpr->getSourceRange());
  C.emitReport(std::move(Report));
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects usage of uninitialized local variable 'ret'", "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
