// /external/upstream-study/SAGEN/result-0224-bugfail-multi-o3mini/test-Uninit-Data-eaa03486d932572dfd1c5f64f9dfebe572ad88c0/checkers/checker4.cpp
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Checkers/Taint.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/Environment.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramState.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramStateTrait.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/MemRegion.h"
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

// Track local scalar variables that have been declared but not definitely written.
REGISTER_MAP_WITH_PROGRAMSTATE(UninitVarMap, const VarDecl*, bool)

namespace {

class SAGenTestChecker : public Checker<check::PostStmt<DeclStmt>,
                                          check::Bind,
                                          check::PreStmt<ReturnStmt>> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker() 
      : BT(new BugType(this, "Uninitialized Variable", "Uninitialized ret usage")) {}

  // Called after a declaration statement is processed.
  void checkPostStmt(const DeclStmt *DS, CheckerContext &C) const;

  // Called when a value is bound to a variable.
  void checkBind(SVal Loc, SVal Val, const Stmt *S, CheckerContext &C) const;

  // Called before a return statement is processed.
  void checkPreStmt(const ReturnStmt *RS, CheckerContext &C) const;
};

static bool shouldTrackUninitLocal(const VarDecl *VD) {
  return VD && VD->hasLocalStorage() && !isa<ParmVarDecl>(VD) &&
         VD->getType()->isScalarType();
}

static const DeclRefExpr *findUninitializedUse(const Expr *E,
                                               ProgramStateRef State) {
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
      if (const DeclRefExpr *Use = findUninitializedUse(ChildExpr, State))
        return Use;
    }
  }
  return nullptr;
}

// Record declarations where the patch-style fix would provide the missing barrier:
// an explicit initializer before any path can reach a return value.
void SAGenTestChecker::checkPostStmt(const DeclStmt *DS, CheckerContext &C) const {
  ProgramStateRef State = C.getState();
  for (const Decl *D : DS->decls()) {
    const auto *VD = dyn_cast<VarDecl>(D);
    if (shouldTrackUninitLocal(VD) && !VD->hasInit())
      State = State->set<UninitVarMap>(VD, false);
  }
  C.addTransition(State);
}

// Any concrete write to a tracked variable closes the uninitialized state on that
// path; paths that skip the write keep the original false state.
void SAGenTestChecker::checkBind(SVal Loc, SVal Val, const Stmt *S,
                                 CheckerContext &C) const {
  ProgramStateRef State = C.getState();
  const MemRegion *Region = Loc.getAsRegion();
  const auto *VR = dyn_cast_or_null<VarRegion>(Region);
  if (!VR) {
    C.addTransition(State);
    return;
  }

  const auto *VD = dyn_cast<VarDecl>(VR->getDecl());
  if (State->get<UninitVarMap>(VD))
    State = State->set<UninitVarMap>(VD, true);

  C.addTransition(State);
}

// Returning a still-unwritten local exposes the same mechanism fixed by the patch:
// a default success value was missing on paths where no later assignment occurred.
void SAGenTestChecker::checkPreStmt(const ReturnStmt *RS, CheckerContext &C) const {
  ProgramStateRef State = C.getState();
  const DeclRefExpr *Use = findUninitializedUse(RS->getRetValue(), State);
  if (!Use) {
    C.addTransition(State);
    return;
  }

  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto report = std::make_unique<PathSensitiveBugReport>(
      *BT, "Uninitialized local variable returned", N);
  report->addRange(Use->getSourceRange());
  C.emitReport(std::move(report));
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects returning local scalar variables before a definite write",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
