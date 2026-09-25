// /external/upstream-study/SAGEN/result-0224-bugfail-multi-o3mini/test-Uninit-Data-eaa03486d932572dfd1c5f64f9dfebe572ad88c0/checkers/checker4.cpp
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

using namespace clang;
using namespace ento;
using namespace taint;

// Tracks local scalar variables that are declared without an initializer.
// The value records whether the current path has performed a later bind.
REGISTER_MAP_WITH_PROGRAMSTATE(UninitVarMap, const VarDecl*, bool)

namespace {

class SAGenTestChecker : public Checker<check::PostStmt<DeclStmt>,
                                          check::Bind,
                                          check::PreStmt<ReturnStmt>> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Uninitialized Variable",
                       "Uninitialized local value returned")) {}

  void checkPostStmt(const DeclStmt *DS, CheckerContext &C) const;
  void checkBind(SVal Loc, SVal Val, const Stmt *S, CheckerContext &C) const;
  void checkPreStmt(const ReturnStmt *RS, CheckerContext &C) const;
};

static bool isTrackableLocalScalar(const VarDecl *VD) {
  if (!VD || !VD->hasLocalStorage() || VD->isStaticLocal())
    return false;

  QualType Ty = VD->getType();
  return !Ty.isNull() && Ty->isScalarType();
}

static const Expr *findUninitializedUse(const Expr *E, ProgramStateRef State) {
  if (!E)
    return nullptr;

  E = E->IgnoreParenImpCasts();
  if (const auto *DRE = dyn_cast<DeclRefExpr>(E)) {
    if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
      const bool *Initialized = State->get<UninitVarMap>(VD);
      if (Initialized && !*Initialized)
        return DRE;
    }
  }

  for (const Stmt *Child : E->children()) {
    if (const auto *ChildExpr = dyn_cast_or_null<Expr>(Child)) {
      if (const Expr *Found = findUninitializedUse(ChildExpr, State))
        return Found;
    }
  }

  return nullptr;
}

void SAGenTestChecker::checkPostStmt(const DeclStmt *DS, CheckerContext &C) const {
  ProgramStateRef State = C.getState();

  for (const Decl *D : DS->decls()) {
    const auto *VD = dyn_cast<VarDecl>(D);
    if (isTrackableLocalScalar(VD) && !VD->hasInit())
      State = State->set<UninitVarMap>(VD, false);
  }

  C.addTransition(State);
}

void SAGenTestChecker::checkBind(SVal Loc, SVal Val, const Stmt *S,
                                 CheckerContext &C) const {
  const MemRegion *MR = Loc.getAsRegion();
  const auto *VR = MR ? dyn_cast<VarRegion>(MR->getBaseRegion()) : nullptr;
  const VarDecl *VD = VR ? VR->getDecl() : nullptr;
  if (!isTrackableLocalScalar(VD))
    return;

  ProgramStateRef State = C.getState();
  if (!State->get<UninitVarMap>(VD))
    return;

  C.addTransition(State->set<UninitVarMap>(VD, true));
}

void SAGenTestChecker::checkPreStmt(const ReturnStmt *RS,
                                    CheckerContext &C) const {
  ProgramStateRef State = C.getState();
  const Expr *Returned = RS->getRetValue();
  const Expr *UninitUse = findUninitializedUse(Returned, State);
  if (!UninitUse)
    return;

  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT, "Uninitialized local variable may be returned", N);
  Report->addRange(UninitUse->getSourceRange());
  C.emitReport(std::move(Report));
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects returning local scalar variables that lack a path initializer",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
