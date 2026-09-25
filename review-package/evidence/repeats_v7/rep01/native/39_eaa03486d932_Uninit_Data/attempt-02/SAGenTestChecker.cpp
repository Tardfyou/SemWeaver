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

// Program state map to keep track of the initialization status of a variable 'ret'.
// The mapping: VarDecl* -> bool (true means initialized, false means uninitialized).
REGISTER_MAP_WITH_PROGRAMSTATE(UninitVarMap, const VarDecl*, bool)

namespace {

class StatusReturnVisitor
    : public RecursiveASTVisitor<StatusReturnVisitor> {
  const VarDecl *Target;
  bool AssignedFromCall = false;
  bool UsedAsBranchCondition = false;
  bool ReturnedDirectly = false;

  bool usesTarget(const Stmt *S) const {
    if (!S)
      return false;
    if (const auto *DRE = dyn_cast<DeclRefExpr>(S->IgnoreImplicit()))
      return DRE->getDecl() == Target;
    for (const Stmt *Child : S->children()) {
      if (usesTarget(Child))
        return true;
    }
    return false;
  }

public:
  explicit StatusReturnVisitor(const VarDecl *Target) : Target(Target) {}

  bool VisitBinaryOperator(const BinaryOperator *BO) {
    if (!BO->isAssignmentOp())
      return true;
    const auto *LHS = dyn_cast<DeclRefExpr>(BO->getLHS()->IgnoreParenImpCasts());
    const auto *RHS = dyn_cast<CallExpr>(BO->getRHS()->IgnoreParenImpCasts());
    if (LHS && LHS->getDecl() == Target && RHS)
      AssignedFromCall = true;
    return true;
  }

  bool VisitIfStmt(const IfStmt *IS) {
    UsedAsBranchCondition |= usesTarget(IS->getCond());
    return true;
  }

  bool VisitReturnStmt(const ReturnStmt *RS) {
    const Expr *Value = RS->getRetValue();
    ReturnedDirectly |= Value &&
        isa<DeclRefExpr>(Value->IgnoreParenImpCasts()) &&
        cast<DeclRefExpr>(Value->IgnoreParenImpCasts())->getDecl() == Target;
    return true;
  }

  bool isStatusReturned() const {
    return AssignedFromCall && UsedAsBranchCondition && ReturnedDirectly;
  }
};

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

// checkPostStmt: Tracks uninitialized integer status values that can be
// conditionally produced by a call and then returned to the caller.
void SAGenTestChecker::checkPostStmt(const DeclStmt *DS, CheckerContext &C) const {
  ProgramStateRef State = C.getState();
  const auto *FD = dyn_cast<FunctionDecl>(C.getLocationContext()->getDecl());
  if (!FD || !FD->hasBody()) {
    C.addTransition(State);
    return;
  }

  for (const Decl *D : DS->decls()) {
    const auto *VD = dyn_cast<VarDecl>(D);
    if (!VD || VD->hasInit() || !VD->getType()->isIntegerType())
      continue;

    StatusReturnVisitor Visitor(VD);
    Visitor.TraverseStmt(const_cast<Stmt *>(FD->getBody()));
    if (Visitor.isStatusReturned())
      State = State->set<UninitVarMap>(VD, false);
  }
  C.addTransition(State);
}

// checkBind: Processes bindings (assignments) to update the initialization status
// of a variable. If a binding to the "ret" variable occurs, mark it as initialized.
void SAGenTestChecker::checkBind(SVal Loc, SVal Val, const Stmt *S, CheckerContext &C) const {
  ProgramStateRef State = C.getState();
  // Try to extract the variable from the left-hand side of the binding.
  if (const Expr *E = dyn_cast<Expr>(S)) {
    if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(E->IgnoreImplicit())) {
      if (const VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
        if (const bool *Initialized = State->get<UninitVarMap>(VD)) {
          if (!*Initialized)
            State = State->set<UninitVarMap>(VD, true);
          C.addTransition(State);
          return;
        }
      }
    }
  }
  C.addTransition(State);
}

// checkPreStmt: Called before a ReturnStmt is processed.
// If the return expression is a direct use of the variable "ret" and it is still uninitialized,
// report a bug.
void SAGenTestChecker::checkPreStmt(const ReturnStmt *RS, CheckerContext &C) const {
  ProgramStateRef State = C.getState();
  const Expr *retExpr = RS->getRetValue();
  if (!retExpr)
    return;

  retExpr = retExpr->IgnoreImplicit();
  if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(retExpr)) {
    if (const VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
      const bool *Initialized = State->get<UninitVarMap>(VD);
      if (Initialized && !(*Initialized)) {
        ExplodedNode *N = C.generateNonFatalErrorNode();
        if (!N)
          return;
        auto report = std::make_unique<PathSensitiveBugReport>(
            *BT, "Uninitialized local variable used as a return value", N);
        report->addRange(retExpr->getSourceRange());
        C.emitReport(std::move(report));
      }
    }
  }
  C.addTransition(State);
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
