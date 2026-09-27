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
#include "llvm/ADT/SmallVector.h"
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
                             check::PostCall,
                             check::PreStmt<ReturnStmt>,
                             check::EndFunction> {
   mutable std::unique_ptr<BugType> BT;

   public:
      SAGenTestChecker() : BT(new BugType(this, "Auto-cleanup pointer may be freed uninitialized", "Memory Management")) {}

      void checkPostStmt(const DeclStmt *DS, CheckerContext &C) const;
      void checkBind(SVal Loc, SVal Val, const Stmt *S, CheckerContext &C) const;
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

// A cleanup callback consumes the tracked object when the callback body
// passes a value derived from its parameter onward to another call (an
// ownership sink such as a deallocator). This models the scope-exit
// ownership handoff behind __attribute__((cleanup)) without depending on
// the callback's spelling.
class RefersToDeclVisitor : public RecursiveASTVisitor<RefersToDeclVisitor> {
public:
  explicit RefersToDeclVisitor(const ValueDecl *D) : Target(D) {}

  bool VisitDeclRefExpr(DeclRefExpr *DRE) {
    if (DRE->getDecl() == Target)
      Found = true;
    return !Found;
  }

  bool found() const { return Found; }

private:
  const ValueDecl *Target;
  bool Found = false;
};

static bool exprRefersToDecl(const Expr *E, const ValueDecl *D) {
  if (!E || !D)
    return false;
  RefersToDeclVisitor V(D);
  V.TraverseStmt(const_cast<Expr *>(E));
  return V.found();
}

class ConsumingCleanupVisitor
    : public RecursiveASTVisitor<ConsumingCleanupVisitor> {
public:
  explicit ConsumingCleanupVisitor(const ParmVarDecl *P) : Param(P) {}

  bool VisitDeclStmt(DeclStmt *DS) {
    for (const Decl *D : DS->decls()) {
      const auto *VD = dyn_cast<VarDecl>(D);
      if (!VD || !VD->hasInit())
        continue;
      // A callback-local derived from the parameter carries the tracked
      // value onward (e.g. "T val = *(T *)ptr;").
      if (exprRefersToDecl(VD->getInit(), Param))
        DerivedLocals.push_back(VD);
    }
    return true;
  }

  bool VisitCallExpr(CallExpr *CE) {
    if (Found)
      return false;
    for (const Expr *Arg : CE->arguments()) {
      if (exprRefersToDecl(Arg, Param)) {
        Found = true;
        return false;
      }
      for (const VarDecl *L : DerivedLocals) {
        if (exprRefersToDecl(Arg, L)) {
          Found = true;
          return false;
        }
      }
    }
    return true;
  }

  bool found() const { return Found; }

private:
  const ParmVarDecl *Param;
  llvm::SmallVector<const VarDecl *, 4> DerivedLocals;
  bool Found = false;
};

bool SAGenTestChecker::hasKfreeCleanup(const VarDecl *VD) {
  if (!VD)
    return false;
  const CleanupAttr *CA = VD->getAttr<CleanupAttr>();
  if (!CA)
    return false;

  const FunctionDecl *FD = CA->getFunctionDecl();
  if (!FD)
    return false;

  // The cleanup callback receives the object's address; require proof from
  // its body that the scope-exit callback consumes the tracked pointer
  // value. Without a resolvable body the ownership relation is unprovable,
  // so stay silent.
  Stmt *Body = FD->getBody();
  if (!Body || FD->getNumParams() != 1)
    return false;

  ConsumingCleanupVisitor V(FD->getParamDecl(0));
  V.TraverseStmt(Body);
  return V.found();
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
