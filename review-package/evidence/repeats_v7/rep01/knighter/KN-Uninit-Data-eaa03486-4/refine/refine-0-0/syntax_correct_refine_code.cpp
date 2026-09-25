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

#include <memory>

using namespace clang;
using namespace ento;
using namespace taint;

// VarDecl* -> initialization state.
// true means that this path has definitely bound a non-undefined value to ret.
REGISTER_MAP_WITH_PROGRAMSTATE(UninitVarMap, const VarDecl *, bool)

namespace {

class SAGenTestChecker
    : public Checker<check::PreStmt<DeclStmt>,
                     check::PostStmt<DeclStmt>,
                     check::Bind,
                     check::PreStmt<ReturnStmt>> {
  mutable std::unique_ptr<BugType> BT;

  static bool isTrackedRet(const VarDecl *VD) {
    // Restrict tracking to automatic local variables. Static and external
    // variables have static storage initialization semantics and are outside
    // this checker's intended bug pattern.
    return VD && VD->getName() == "ret" && VD->hasLocalStorage();
  }

  static const VarDecl *getTrackedRetFromLocation(SVal Loc) {
    // The location being bound is more reliable than the callback's Stmt.
    // For "ret = f()", S is normally a BinaryOperator, not a DeclRefExpr.
    const auto *VR = dyn_cast_or_null<VarRegion>(Loc.getAsRegion());
    if (!VR)
      return nullptr;

    const VarDecl *VD = VR->getDecl();
    return isTrackedRet(VD) ? VD : nullptr;
  }

  static const VarDecl *getDirectRetFromExpr(const Expr *E) {
    if (!E)
      return nullptr;

    E = E->IgnoreParenImpCasts();
    const auto *DRE = dyn_cast<DeclRefExpr>(E);
    if (!DRE)
      return nullptr;

    const auto *VD = dyn_cast<VarDecl>(DRE->getDecl());
    return isTrackedRet(VD) ? VD : nullptr;
  }

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Uninitialized Variable",
                       "Uninitialized ret usage")) {}

  void checkPreStmt(const DeclStmt *DS, CheckerContext &C) const;
  void checkPostStmt(const DeclStmt *DS, CheckerContext &C) const;
  void checkBind(SVal Loc, SVal Val, const Stmt *,
                 CheckerContext &C) const;
  void checkPreStmt(const ReturnStmt *RS, CheckerContext &C) const;
};

// Register ret before evaluating declaration initializers. This lets checkBind()
// observe assignments to ret from another declarator in the same DeclStmt, e.g.
// "int ret, x = (ret = get_value());".
void SAGenTestChecker::checkPreStmt(const DeclStmt *DS,
                                    CheckerContext &C) const {
  ProgramStateRef State = C.getState();
  ProgramStateRef NewState = State;

  for (const Decl *D : DS->decls()) {
    const auto *VD = dyn_cast<VarDecl>(D);
    if (!isTrackedRet(VD))
      continue;

    // Start conservatively. A declaration initializer or later assignment will
    // establish initialization through checkBind()/checkPostStmt().
    NewState = NewState->set<UninitVarMap>(VD, false);
  }

  if (NewState != State)
    C.addTransition(NewState);
}

// A declaration with an initializer is initialized by language semantics even
// if its initializer is not represented by a direct bind in the current model.
void SAGenTestChecker::checkPostStmt(const DeclStmt *DS,
                                     CheckerContext &C) const {
  ProgramStateRef State = C.getState();
  ProgramStateRef NewState = State;

  for (const Decl *D : DS->decls()) {
    const auto *VD = dyn_cast<VarDecl>(D);
    if (!isTrackedRet(VD) || !VD->hasInit())
      continue;

    NewState = NewState->set<UninitVarMap>(VD, true);
  }

  if (NewState != State)
    C.addTransition(NewState);
}

// Mark ret initialized when the analyzer binds a defined or unknown value to
// ret's actual storage region. This correctly recognizes:
//
//   ret = mas_store_gfp(&mas, entry, map->alloc_flags);
//
// even when mas_store_gfp() is not modeled or inlined.
void SAGenTestChecker::checkBind(SVal Loc, SVal Val, const Stmt *,
                                 CheckerContext &C) const {
  const VarDecl *VD = getTrackedRetFromLocation(Loc);
  if (!VD)
    return;

  ProgramStateRef State = C.getState();

  // A binding of UndefinedVal does not prove initialization. This preserves
  // reports for paths that merely propagate an uninitialized value.
  if (Val.isUndef())
    return;

  // Only update declarations already registered by checkPreStmt(). This avoids
  // accidentally tracking parameters or unrelated variables named ret.
  const bool *Initialized = State->get<UninitVarMap>(VD);
  if (!Initialized || *Initialized)
    return;

  C.addTransition(State->set<UninitVarMap>(VD, true));
}

void SAGenTestChecker::checkPreStmt(const ReturnStmt *RS,
                                    CheckerContext &C) const {
  const VarDecl *VD = getDirectRetFromExpr(RS->getRetValue());
  if (!VD)
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
  Report->addRange(RS->getRetValue()->getSourceRange());
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
