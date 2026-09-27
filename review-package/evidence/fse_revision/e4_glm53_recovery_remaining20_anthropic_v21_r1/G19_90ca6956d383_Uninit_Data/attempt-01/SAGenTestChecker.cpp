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
#include "clang/AST/Attr.h"
#include "llvm/ADT/ImmutableMap.h"

using namespace clang;
using namespace ento;
using namespace taint;

// Program state: Track auto-cleanup pointer locals (kfree) and whether initialized.
REGISTER_MAP_WITH_PROGRAMSTATE(TrackedAutoCleanup, const VarDecl *, bool)

namespace {

class SAGenTestChecker : public Checker<
                             check::PostStmt<DeclStmt>,
                             check::Bind,
                             check::PreCall,
                             check::PostCall,
                             check::PreStmt<ReturnStmt>,
                             check::EndFunction> {
   mutable std::unique_ptr<BugType> BT;

   public:
      SAGenTestChecker() : BT(new BugType(this, "Auto-cleanup pointer may be freed uninitialized", "Memory Management")) {}

      void checkPostStmt(const DeclStmt *DS, CheckerContext &C) const;
      void checkBind(SVal Loc, SVal Val, const Stmt *S, CheckerContext &C) const;
      void checkPreCall(const CallEvent &Call, CheckerContext &C) const;
      void checkPostCall(const CallEvent &Call, CheckerContext &C) const;
      void checkPreStmt(const ReturnStmt *RS, CheckerContext &C) const;
      void checkEndFunction(const ReturnStmt *RS, CheckerContext &C) const;

   private:
      static bool hasKfreeCleanup(const VarDecl *VD);
      void reportUninitializedAtExit(const Stmt *Trigger, CheckerContext &C) const;
};

static PathDiagnosticLocation getDeclLoc(const VarDecl *VD, CheckerContext &C) {
  return PathDiagnosticLocation::createBegin(VD, C.getSourceManager());
}

bool SAGenTestChecker::hasKfreeCleanup(const VarDecl *VD) {
  if (!VD)
    return false;
  const CleanupAttr *CA = VD->getAttr<CleanupAttr>();
  if (!CA)
    return false;

  if (const FunctionDecl *FD = CA->getFunctionDecl()) {
    if (const IdentifierInfo *II = FD->getIdentifier()) {
      return II->getName().equals("kfree");
    }
  }
  // If the registered cleanup callback did not resolve to a FunctionDecl, we
  // cannot name-check it; the attribute still registers a cleanup that runs at
  // scope exit, so keep tracking such pointer locals.
  return true;
}

void SAGenTestChecker::checkPostStmt(const DeclStmt *DS, CheckerContext &C) const {
  ProgramStateRef State = C.getState();

  for (const Decl *D : DS->decls()) {
    const auto *VD = dyn_cast<VarDecl>(D);
    if (!VD)
      continue;

    // Only track automatic local pointers with cleanup(kfree) and no initializer.
    if (!VD->hasLocalStorage())
      continue;

    if (VD->getStorageDuration() != SD_Automatic)
      continue;

    QualType QT = VD->getType();
    if (QT.isNull() || !QT->isPointerType())
      continue;

    if (!hasKfreeCleanup(VD))
      continue;

    // If it has an initializer (even non-NULL), treat as initialized; skip tracking.
    if (VD->hasInit())
      continue;

    // Start tracking as "not initialized" (false).
    State = State->set<TrackedAutoCleanup>(VD, false);
  }

  if (State != C.getState())
    C.addTransition(State);
}

void SAGenTestChecker::checkBind(SVal Loc, SVal Val, const Stmt *S, CheckerContext &C) const {
  ProgramStateRef State = C.getState();
  const MemRegion *MR = Loc.getAsRegion();
  if (!MR)
    return;

  if (const auto *VR = dyn_cast<VarRegion>(MR->getBaseRegion())) {
    const VarDecl *VD = VR->getDecl();
    if (!VD)
      return;

    const bool *Tracked = State->get<TrackedAutoCleanup>(VD);
    if (Tracked) {
      // Any assignment counts as initialization.
      State = State->set<TrackedAutoCleanup>(VD, true);
      C.addTransition(State);
    }
  }
}

// The dangerous use is the variable's own registered cleanup callback running
// on a pointer that this path has never assigned. Bind the report to that
// call: the argument must be the tracked variable itself and the callee must
// be the function the variable's CleanupAttr registered.
void SAGenTestChecker::checkPreCall(const CallEvent &Call, CheckerContext &C) const {
  if (Call.getNumArgs() == 0)
    return;

  const VarDecl *VD = nullptr;
  if (const Expr *ArgE = Call.getArgExpr(0)) {
    const Expr *E = ArgE->IgnoreParenImpCasts();
    if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
      VD = dyn_cast<VarDecl>(DRE->getDecl());
  }
  if (!VD) {
    if (const MemRegion *MR = Call.getArgSVal(0).getAsRegion())
      if (const auto *VR = dyn_cast<VarRegion>(MR->getBaseRegion()))
        VD = VR->getDecl();
  }
  if (!VD)
    return;

  ProgramStateRef State = C.getState();
  const bool *Tracked = State->get<TrackedAutoCleanup>(VD);
  if (!Tracked || *Tracked)
    return;

  const CleanupAttr *CA = VD->getAttr<CleanupAttr>();
  if (!CA)
    return;
  if (const FunctionDecl *RegFD = CA->getFunctionDecl()) {
    const auto *CalledFD = dyn_cast<FunctionDecl>(Call.getDecl());
    if (!CalledFD || CalledFD->getCanonicalDecl() != RegFD->getCanonicalDecl())
      return;
  }

  // Mark the variable as handled so the exit checks do not duplicate this
  // alert on the same path.
  ProgramStateRef Next = State->set<TrackedAutoCleanup>(VD, true);
  ExplodedNode *N = C.generateNonFatalErrorNode(Next);
  if (!N)
    return;

  auto R = std::make_unique<PathSensitiveBugReport>(
      *BT,
      "Auto-cleanup pointer is freed by its registered cleanup callback"
      " without ever being assigned; initialize to NULL",
      N);

  if (const Expr *ArgE = Call.getArgExpr(0))
    R->addRange(ArgE->getSourceRange());
  R->addNote("Declared here without initializer", getDeclLoc(VD, C));
  C.emitReport(std::move(R));
}

void SAGenTestChecker::checkPostCall(const CallEvent &Call, CheckerContext &C) const {
  ProgramStateRef State = C.getState();

  // If an argument is &var where var is tracked, conservatively treat it as initialized.
  for (unsigned i = 0, e = Call.getNumArgs(); i < e; ++i) {
    const Expr *ArgE = Call.getArgExpr(i);
    if (!ArgE)
      continue;

    const Expr *E = ArgE->IgnoreParenCasts();
    const UnaryOperator *UO = dyn_cast<UnaryOperator>(E);
    if (!UO || UO->getOpcode() != UO_AddrOf)
      continue;

    const Expr *Sub = UO->getSubExpr();
    if (!Sub)
      continue;
    Sub = Sub->IgnoreParenCasts();

    if (const auto *DRE = dyn_cast<DeclRefExpr>(Sub)) {
      const auto *VD = dyn_cast<VarDecl>(DRE->getDecl());
      if (!VD)
        continue;

      const bool *Tracked = State->get<TrackedAutoCleanup>(VD);
      if (Tracked && *Tracked == false) {
        State = State->set<TrackedAutoCleanup>(VD, true);
      }
    }
  }

  if (State != C.getState())
    C.addTransition(State);
}

void SAGenTestChecker::reportUninitializedAtExit(const Stmt *Trigger, CheckerContext &C) const {
  ProgramStateRef State = C.getState();
  auto Map = State->get<TrackedAutoCleanup>();

  for (auto I = Map.begin(), E = Map.end(); I != E; ++I) {
    const VarDecl *VD = I->first;
    bool Inited = I->second;
    if (!Inited) {
      ExplodedNode *N = C.generateNonFatalErrorNode();
      if (!N)
        continue;

      auto R = std::make_unique<PathSensitiveBugReport>(
          *BT, "Auto-cleanup pointer may be freed uninitialized; initialize to NULL", N);

      if (Trigger)
        R->addRange(Trigger->getSourceRange());

      R->addNote("Declared here without initializer", getDeclLoc(VD, C));
      C.emitReport(std::move(R));
    }
  }
}

void SAGenTestChecker::checkPreStmt(const ReturnStmt *RS, CheckerContext &C) const {
  // On any return, if there is an uninitialized tracked auto-cleanup ptr, report.
  reportUninitializedAtExit(RS, C);
}

void SAGenTestChecker::checkEndFunction(const ReturnStmt *RS, CheckerContext &C) const {
  // Catch fallthrough to end of function (no explicit return).
  reportUninitializedAtExit(RS, C);
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects auto-cleanup (kfree) pointers not initialized to NULL before early exit",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
