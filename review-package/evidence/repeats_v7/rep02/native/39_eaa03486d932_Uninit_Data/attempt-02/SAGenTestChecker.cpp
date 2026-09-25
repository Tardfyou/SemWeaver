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
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtVisitor.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/StaticAnalyzer/Checkers/utility.h"

#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"

using namespace clang;
using namespace ento;
using namespace taint;

// Program state map for automatic locals declared without an initializer.
// The mapping: VarDecl* -> bool (true means initialized, false means uninitialized).
REGISTER_MAP_WITH_PROGRAMSTATE(UninitVarMap, const VarDecl*, bool)

namespace {

class SAGenTestChecker : public Checker<check::PostStmt<DeclStmt>,
                                          check::Bind,
                                          check::PreStmt<DeclRefExpr>> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Uninitialized Variable", "Uninitialized value usage")) {}

  // Records automatic locals whose declaration does not write a value.
  void checkPostStmt(const DeclStmt *DS, CheckerContext &C) const;

  // Marks a tracked local initialized when its storage receives a value.
  void checkBind(SVal Loc, SVal Val, const Stmt *S, CheckerContext &C) const;

  // Reports an evaluated read of a local still lacking a reaching write.
  void checkPreStmt(const DeclRefExpr *DRE, CheckerContext &C) const;
};

// Records automatic locals whose declaration has not initialized their storage.
void SAGenTestChecker::checkPostStmt(const DeclStmt *DS, CheckerContext &C) const {
  ProgramStateRef State = C.getState();
  for (const Decl *D : DS->decls()) {
    const VarDecl *VD = dyn_cast<VarDecl>(D);
    if (VD && VD->getStorageDuration() == SD_Automatic && !VD->hasInit())
      State = State->set<UninitVarMap>(VD, false);
  }
  C.addTransition(State);
}

// Marks a tracked local initialized when a value is bound to its storage region.
void SAGenTestChecker::checkBind(SVal Loc, SVal Val, const Stmt *S, CheckerContext &C) const {
  ProgramStateRef State = C.getState();
  const MemRegion *Region = Loc.getAsRegion();
  const VarRegion *VR = dyn_cast_or_null<VarRegion>(Region);
  const VarDecl *VD = VR ? VR->getDecl() : nullptr;
  if (VD) {
    const bool *Initialized = State->get<UninitVarMap>(VD);
    if (Initialized && !*Initialized)
      State = State->set<UninitVarMap>(VD, true);
  }
  C.addTransition(State);
}

// Reports an evaluated read of an automatic local that has no reaching write.
void SAGenTestChecker::checkPreStmt(const DeclRefExpr *DRE,
                                    CheckerContext &C) const {
  const auto Parents = C.getASTContext().getParents(*DRE);
  if (Parents.empty())
    return;

  const auto *Cast = Parents[0].get<ImplicitCastExpr>();
  if (!Cast || Cast->getCastKind() != CK_LValueToRValue)
    return;

  ProgramStateRef State = C.getState();
  const VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl());
  const bool *Initialized = VD ? State->get<UninitVarMap>(VD) : nullptr;
  if (!Initialized || *Initialized)
    return;

  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;
  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT, "Uninitialized local variable read", N);
  Report->addRange(DRE->getSourceRange());
  C.emitReport(std::move(Report));
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker", 
      "Detects usage of uninitialized local variable 'ret'", 
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
