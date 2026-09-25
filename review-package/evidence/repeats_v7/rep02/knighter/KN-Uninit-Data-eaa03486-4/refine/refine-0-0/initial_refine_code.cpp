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

#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"

using namespace clang;
using namespace ento;
using namespace taint;

// Tracks automatic local variables named "ret" by their actual storage region.
// false means the region has not been definitely initialized on this path;
// true means it has been assigned on this path.
REGISTER_MAP_WITH_PROGRAMSTATE(RetInitializationMap, const MemRegion *, bool)

namespace {

class SAGenTestChecker : public Checker<check::PostStmt<DeclStmt>,
                                        check::Bind,
                                        check::PreStmt<ReturnStmt>> {
  mutable std::unique_ptr<BugType> BT;

  static const MemRegion *getVarRegion(const VarDecl *VD,
                                       ProgramStateRef State,
                                       CheckerContext &C);

  static const VarDecl *getReturnedRetDecl(const Expr *E);

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Uninitialized Variable",
                       "Uninitialized ret usage")) {}

  void checkPostStmt(const DeclStmt *DS, CheckerContext &C) const;
  void checkBind(SVal Loc, SVal Val, const Stmt *S,
                 CheckerContext &C) const;
  void checkPreStmt(const ReturnStmt *RS, CheckerContext &C) const;
};

const MemRegion *SAGenTestChecker::getVarRegion(const VarDecl *VD,
                                                ProgramStateRef State,
                                                CheckerContext &C) {
  SVal Loc = State->getLValue(VD, C.getLocationContext());
  return Loc.getAsRegion();
}

const VarDecl *SAGenTestChecker::getReturnedRetDecl(const Expr *E) {
  if (!E)
    return nullptr;

  // Parentheses and both implicit and explicit casts preserve a read of ret.
  E = E->IgnoreParenImpCasts();
  while (const auto *CE = dyn_cast<CastExpr>(E))
    E = CE->getSubExpr()->IgnoreParenImpCasts();

  const auto *DRE = dyn_cast<DeclRefExpr>(E);
  if (!DRE)
    return nullptr;

  return dyn_cast<VarDecl>(DRE->getDecl());
}

// Record a local ret declaration that has no initializer.
void SAGenTestChecker::checkPostStmt(const DeclStmt *DS,
                                     CheckerContext &C) const {
  ProgramStateRef State = C.getState();
  bool StateChanged = false;

  for (const Decl *D : DS->decls()) {
    const auto *VD = dyn_cast<VarDecl>(D);
    if (!VD || VD->getName() != "ret" || VD->hasInit() ||
        !VD->hasLocalStorage())
      continue;

    const MemRegion *Region = getVarRegion(VD, State, C);
    if (!Region)
      continue;

    State = State->set<RetInitializationMap>(Region, false);
    StateChanged = true;
  }

  if (StateChanged)
    C.addTransition(State);
}

// Loc, rather than S, identifies the storage being written. S is merely the
// statement that triggered the bind and is commonly a BinaryOperator or call.
void SAGenTestChecker::checkBind(SVal Loc, SVal Val, const Stmt *S,
                                 CheckerContext &C) const {
  ProgramStateRef State = C.getState();
  const MemRegion *Region = Loc.getAsRegion();
  if (!Region)
    return;

  const bool *Initialized = State->get<RetInitializationMap>(Region);
  if (!Initialized || *Initialized)
    return;

  // This exact tracked region has now received a value on the current path.
  State = State->set<RetInitializationMap>(Region, true);
  C.addTransition(State);
}

// Report only when the returned ret expression is still uninitialized on the
// current path.
void SAGenTestChecker::checkPreStmt(const ReturnStmt *RS,
                                    CheckerContext &C) const {
  const Expr *RetExpr = RS->getRetValue();
  const VarDecl *VD = getReturnedRetDecl(RetExpr);
  if (!VD || VD->getName() != "ret" || !VD->hasLocalStorage())
    return;

  ProgramStateRef State = C.getState();
  const MemRegion *Region = getVarRegion(VD, State, C);
  if (!Region)
    return;

  const bool *Initialized = State->get<RetInitializationMap>(Region);
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
      "Detects usage of uninitialized local variable 'ret'",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
