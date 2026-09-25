// /external/upstream-study/SAGEN/result-0224-bugfail-multi-o3mini/test-Double-Free-39b13dce1a91cdfc3bec9238f9e89094551bd428/checkers/checker0.cpp
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/Stmt.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include <memory>

using namespace clang;
using namespace ento;

namespace {

class SAGenTestChecker : public Checker<check::PreCall> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Redundant Cleanup Call", "Double Free")) {}

  void checkPreCall(const CallEvent &Call, CheckerContext &C) const;

private:
  void reportRedundantCleanup(const CallEvent &Call, CheckerContext &C) const;
};

static const CallExpr *getDirectCall(const Expr *Expr) {
  return Expr ? dyn_cast<CallExpr>(Expr->IgnoreParenImpCasts()) : nullptr;
}

static const FunctionDecl *getFunctionTarget(const Expr *Expr) {
  if (!Expr)
    return nullptr;

  Expr = Expr->IgnoreParenImpCasts();
  if (const auto *Ref = dyn_cast<DeclRefExpr>(Expr))
    return dyn_cast<FunctionDecl>(Ref->getDecl());
  if (const auto *Unary = dyn_cast<UnaryOperator>(Expr))
    if (Unary->getOpcode() == UO_AddrOf)
      return getFunctionTarget(Unary->getSubExpr());
  return nullptr;
}

static const ValueDecl *getReferencedValue(const Expr *Expr) {
  if (!Expr)
    return nullptr;

  Expr = Expr->IgnoreParenImpCasts();
  if (const auto *Ref = dyn_cast<DeclRefExpr>(Expr))
    return dyn_cast<ValueDecl>(Ref->getDecl());
  return nullptr;
}

static const CallExpr *getResetRegistration(const Expr *Condition,
                                            bool &FailureIsThen) {
  FailureIsThen = true;
  if (!Condition)
    return nullptr;

  Condition = Condition->IgnoreParenImpCasts();
  if (const auto *Unary = dyn_cast<UnaryOperator>(Condition)) {
    if (Unary->getOpcode() != UO_LNot)
      return nullptr;
    FailureIsThen = false;
    Condition = Unary->getSubExpr()->IgnoreParenImpCasts();
  }

  const CallExpr *Registration = getDirectCall(Condition);
  const FunctionDecl *Callee =
      Registration ? Registration->getDirectCallee() : nullptr;
  if (!Callee || Callee->getName() != "__devm_add_action_or_reset")
    return nullptr;
  return Registration;
}

void SAGenTestChecker::checkPreCall(const CallEvent &Call,
                                    CheckerContext &C) const {
  const Expr *OriginExpr = Call.getOriginExpr();
  const CallExpr *CleanupCall = getDirectCall(OriginExpr);
  if (!CleanupCall || CleanupCall->getNumArgs() != 1)
    return;

  const FunctionDecl *Cleanup = CleanupCall->getDirectCallee();
  const ValueDecl *CleanupObject = getReferencedValue(CleanupCall->getArg(0));
  if (!Cleanup || !CleanupObject)
    return;

  const Stmt *Current = OriginExpr;
  ASTContext &Ctx = C.getASTContext();
  while (Current) {
    auto Parents = Ctx.getParents(*Current);
    auto ParentIt = Parents.begin();
    if (ParentIt == Parents.end())
      return;

    const Stmt *Parent = ParentIt->get<Stmt>();
    ++ParentIt;
    if (!Parent || ParentIt != Parents.end())
      return;

    if (const auto *If = dyn_cast<IfStmt>(Parent)) {
      bool FailureIsThen = false;
      const CallExpr *Registration =
          getResetRegistration(If->getCond(), FailureIsThen);
      const Stmt *FailureArm = FailureIsThen ? If->getThen() : If->getElse();
      if (Registration && Current == FailureArm &&
          Registration->getNumArgs() >= 3 &&
          getFunctionTarget(Registration->getArg(1)) == Cleanup &&
          getReferencedValue(Registration->getArg(2)) == CleanupObject) {
        reportRedundantCleanup(Call, C);
        return;
      }
    }

    Current = Parent;
  }
}

void SAGenTestChecker::reportRedundantCleanup(const CallEvent &Call,
                                              CheckerContext &C) const {
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT, "Cleanup repeats a reset action for the same object", N);
  Report->addRange(Call.getSourceRange());
  C.emitReport(std::move(Report));
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker", 
      "Detects redundant cleanup call in error handling leading to double free", "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
