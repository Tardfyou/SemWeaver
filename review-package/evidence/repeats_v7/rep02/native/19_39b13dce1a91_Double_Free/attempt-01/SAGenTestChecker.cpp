// /external/upstream-study/SAGEN/result-0224-bugfail-multi-o3mini/test-Double-Free-39b13dce1a91cdfc3bec9238f9e89094551bd428/checkers/checker0.cpp
#include "clang/AST/Stmt.h"
#include "clang/Lex/Lexer.h"
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
#include "clang/AST/Expr.h"

using namespace clang;
using namespace ento;
using namespace taint;

namespace {

class SAGenTestChecker : public Checker<check::PreCall> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
    : BT(new BugType(this, "Redundant Cleanup Call", "Double Free")) {}

  void checkPreCall(const CallEvent &Call, CheckerContext &C) const;

private:
  static const FunctionDecl *referencedFunction(const Expr *E);
  static const ValueDecl *referencedValue(const Expr *E);
  static bool containsStmt(const Stmt *Root, const Stmt *Needle);
  static bool isRegisteredCleanupFailure(const IfStmt *If,
                                         const CallExpr *Cleanup);
  void reportRedundantCleanup(const CallEvent &Call, CheckerContext &C) const;
};

const FunctionDecl *SAGenTestChecker::referencedFunction(const Expr *E) {
  E = E->IgnoreParenImpCasts();
  const auto *DRE = dyn_cast<DeclRefExpr>(E);
  return DRE ? dyn_cast<FunctionDecl>(DRE->getDecl()) : nullptr;
}

const ValueDecl *SAGenTestChecker::referencedValue(const Expr *E) {
  E = E->IgnoreParenImpCasts();
  const auto *DRE = dyn_cast<DeclRefExpr>(E);
  return DRE ? dyn_cast<ValueDecl>(DRE->getDecl()) : nullptr;
}

bool SAGenTestChecker::containsStmt(const Stmt *Root, const Stmt *Needle) {
  if (!Root)
    return false;
  if (Root == Needle)
    return true;
  for (const Stmt *Child : Root->children())
    if (containsStmt(Child, Needle))
      return true;
  return false;
}

bool SAGenTestChecker::isRegisteredCleanupFailure(const IfStmt *If,
                                                   const CallExpr *Cleanup) {
  const Expr *Condition = If->getCond()->IgnoreParenImpCasts();
  bool FailureInThen = true;
  if (const auto *Not = dyn_cast<UnaryOperator>(Condition)) {
    if (Not->getOpcode() != UO_LNot)
      return false;
    Condition = Not->getSubExpr()->IgnoreParenImpCasts();
    FailureInThen = false;
  }

  const auto *Registration = dyn_cast<CallExpr>(Condition);
  if (!Registration || Registration->getNumArgs() < 3)
    return false;

  const FunctionDecl *RegistrationDecl = Registration->getDirectCallee();
  if (!RegistrationDecl ||
      RegistrationDecl->getName() != "__devm_add_action_or_reset")
    return false;

  const FunctionDecl *CleanupDecl = Cleanup->getDirectCallee();
  if (!CleanupDecl || Cleanup->getNumArgs() != 1 ||
      referencedFunction(Registration->getArg(1)) != CleanupDecl)
    return false;

  const ValueDecl *RegisteredData = referencedValue(Registration->getArg(2));
  const ValueDecl *CleanupData = referencedValue(Cleanup->getArg(0));
  if (!RegisteredData || RegisteredData != CleanupData)
    return false;

  const Stmt *FailureBranch = FailureInThen ? If->getThen() : If->getElse();
  return containsStmt(FailureBranch, Cleanup);
}

void SAGenTestChecker::checkPreCall(const CallEvent &Call, CheckerContext &C) const {
  const Expr *OriginExpr = Call.getOriginExpr();
  const auto *Cleanup = OriginExpr
      ? dyn_cast<CallExpr>(OriginExpr->IgnoreParenImpCasts())
      : nullptr;
  if (!Cleanup)
    return;

  const FunctionDecl *CleanupDecl = Cleanup->getDirectCallee();
  if (!CleanupDecl ||
      CleanupDecl->getName() != "scmi_debugfs_common_cleanup")
    return;

  const IfStmt *ParentIf = findSpecificTypeInParents<IfStmt>(OriginExpr, C);
  if (!ParentIf || !isRegisteredCleanupFailure(ParentIf, Cleanup))
    return;

  reportRedundantCleanup(Call, C);
}

void SAGenTestChecker::reportRedundantCleanup(const CallEvent &Call, CheckerContext &C) const {
  // Generate a non-fatal error node.
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  // Report the bug with a clear and short diagnostic message.
  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT, "Redundant cleanup call leads to double free", N);
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
