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

// Free primitives: a cleanup callback with these semantics releases the
// pointed-to object, so an indeterminate pointer value reaches the free when
// the variable is never assigned.
static bool isKfreePrimitive(const FunctionDecl *FD) {
  if (!FD)
    return false;
  const IdentifierInfo *II = FD->getIdentifier();
  if (!II)
    return false;
  llvm::StringRef Name = II->getName();
  return Name.equals("kfree") || Name.equals("kvfree") ||
         Name.equals("kfree_sensitive");
}

// Cleanup wrappers (single parameter) whose body forwards to a free primitive;
// the freeing relation survives a consistent rename of the wrapper itself.
class KfreeCallVisitor : public RecursiveASTVisitor<KfreeCallVisitor> {
public:
  bool FoundKfreeCall = false;
  bool VisitCallExpr(const CallExpr *CE) {
    if (isKfreePrimitive(CE->getDirectCallee()))
      FoundKfreeCall = true;
    return true;
  }
};

static bool isKfreeLikeCleanup(const FunctionDecl *FD) {
  if (!FD)
    return false;
  if (isKfreePrimitive(FD))
    return true;
  if (!FD->hasBody() || FD->getNumParams() != 1)
    return false;
  KfreeCallVisitor V;
  V.TraverseStmt(const_cast<Stmt *>(FD->getBody()));
  return V.FoundKfreeCall;
}

bool SAGenTestChecker::hasKfreeCleanup(const VarDecl *VD) {
  if (!VD)
    return false;
  const CleanupAttr *CA = VD->getAttr<CleanupAttr>();
  if (!CA)
    return false;
  // If we cannot resolve the function decl, be conservative and do not track.
  return isKfreeLikeCleanup(CA->getFunctionDecl());
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

void SAGenTestChecker::checkPreCall(const CallEvent &Call, CheckerContext &C) const {
  // Consuming the indeterminate value of a never-assigned auto-cleanup pointer
  // is the same defect as freeing it at scope exit.
  for (unsigned i = 0, e = Call.getNumArgs(); i < e; ++i) {
    const Expr *ArgE = Call.getArgExpr(i);
    if (!ArgE)
      continue;
    const auto *DRE = dyn_cast<DeclRefExpr>(ArgE->IgnoreParenImpCasts());
    if (!DRE)
      continue;
    const auto *VD = dyn_cast<VarDecl>(DRE->getDecl());
    if (!VD)
      continue;
    ProgramStateRef State = C.getState();
    const bool *Tracked = State->get<TrackedAutoCleanup>(VD);
    if (!Tracked || *Tracked)
      continue;
    ExplodedNode *N = C.generateNonFatalErrorNode();
    if (!N)
      continue;
    auto R = std::make_unique<PathSensitiveBugReport>(
        *BT, "Auto-cleanup pointer may be used uninitialized; initialize to NULL", N);
    R->addRange(ArgE->getSourceRange());
    R->addNote("Declared here without initializer", getDeclLoc(VD, C));
    C.emitReport(std::move(R));
  }
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
  // Catch fallthrough to end of function (no explicit return); explicit
  // returns are already reported by checkPreStmt to avoid duplicates.
  if (RS)
    return;
  reportUninitializedAtExit(nullptr, C);
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
