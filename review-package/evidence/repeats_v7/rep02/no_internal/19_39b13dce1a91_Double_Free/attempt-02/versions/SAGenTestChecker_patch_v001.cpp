// /external/upstream-study/SAGEN/result-0224-bugfail-multi-o3mini/test-Double-Free-39b13dce1a91cdfc3bec9238f9e89094551bd428/checkers/checker0.cpp
#include "clang/AST/Stmt.h"
#include "clang/Lex/Lexer.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Checkers/Taint.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
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
  static bool containsStmt(const Stmt *Root, const Stmt *Needle);
  static bool refersToSameObject(const Expr *Left, const Expr *Right);
  static bool hasRedundantCleanup(const Stmt *Root, const Expr *Action,
                                  const Expr *Context);
  void reportRedundantCleanup(const CallEvent &Call, CheckerContext &C) const;
};

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

bool SAGenTestChecker::refersToSameObject(const Expr *Left, const Expr *Right) {
  const auto *LeftRef = dyn_cast<DeclRefExpr>(Left->IgnoreParenImpCasts());
  const auto *RightRef = dyn_cast<DeclRefExpr>(Right->IgnoreParenImpCasts());
  return LeftRef && RightRef &&
         LeftRef->getDecl()->getCanonicalDecl() ==
             RightRef->getDecl()->getCanonicalDecl();
}

bool SAGenTestChecker::hasRedundantCleanup(const Stmt *Root,
                                           const Expr *Action,
                                           const Expr *Context) {
  if (!Root)
    return false;

  if (const auto *Cleanup = dyn_cast<CallExpr>(Root))
    if (Cleanup->getNumArgs() == 1 &&
        refersToSameObject(Cleanup->getCallee(), Action) &&
        refersToSameObject(Cleanup->getArg(0), Context))
      return true;

  for (const Stmt *Child : Root->children())
    if (hasRedundantCleanup(Child, Action, Context))
      return true;
  return false;
}

void SAGenTestChecker::checkPreCall(const CallEvent &Call, CheckerContext &C) const {
  const Expr *OriginExpr = Call.getOriginExpr();
  const auto *Registration = dyn_cast_or_null<CallExpr>(OriginExpr);
  if (!Registration || Registration->getNumArgs() < 3 ||
      !ExprHasName(Registration->getCallee(),
                   "devm_add_action_or_reset", C))
    return;

  const IfStmt *ParentIf = findSpecificTypeInParents<IfStmt>(OriginExpr, C);
  if (!ParentIf || !ParentIf->getCond())
    return;

  const Expr *Condition = ParentIf->getCond()->IgnoreParenImpCasts();
  bool FailureIsThen = true;
  if (const auto *Not = dyn_cast<UnaryOperator>(Condition)) {
    if (Not->getOpcode() != UO_LNot)
      return;
    Condition = Not->getSubExpr()->IgnoreParenImpCasts();
    FailureIsThen = false;
  }
  if (Condition != OriginExpr->IgnoreParenImpCasts())
    return;

  const Stmt *FailureBranch =
      FailureIsThen ? ParentIf->getThen() : ParentIf->getElse();
  if (!hasRedundantCleanup(FailureBranch, Registration->getArg(1),
                           Registration->getArg(2)))
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
