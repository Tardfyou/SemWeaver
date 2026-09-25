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
  static bool containsStmt(const Stmt *Container, const Stmt *Needle) {
    if (!Container)
      return false;
    if (Container == Needle)
      return true;

    for (const Stmt *Child : Container->children())
      if (containsStmt(Child, Needle))
        return true;
    return false;
  }

  static bool refersToSameObject(const Expr *Left, const Expr *Right) {
    if (!Left || !Right)
      return false;

    const auto *LeftRef = dyn_cast<DeclRefExpr>(Left->IgnoreParenImpCasts());
    const auto *RightRef = dyn_cast<DeclRefExpr>(Right->IgnoreParenImpCasts());
    return LeftRef && RightRef &&
           LeftRef->getDecl()->getCanonicalDecl() ==
               RightRef->getDecl()->getCanonicalDecl();
  }

  /// Report the error when a redundant cleanup call is detected.
  void reportRedundantCleanup(const CallEvent &Call, CheckerContext &C) const;
};

void SAGenTestChecker::checkPreCall(const CallEvent &Call, CheckerContext &C) const {
  const Expr *OriginExpr = Call.getOriginExpr();
  const auto *CleanupCall = dyn_cast_or_null<CallExpr>(OriginExpr);
  if (!CleanupCall || CleanupCall->getNumArgs() != 1)
    return;

  if (!ExprHasName(CleanupCall, "scmi_debugfs_common_cleanup", C))
    return;

  const IfStmt *ParentIf = findSpecificTypeInParents<IfStmt>(OriginExpr, C);
  if (!ParentIf || !containsStmt(ParentIf->getThen(), OriginExpr))
    return;

  const auto *Registration =
      dyn_cast<CallExpr>(ParentIf->getCond()->IgnoreParenImpCasts());
  if (!Registration || Registration->getNumArgs() < 3 ||
      !ExprHasName(Registration, "devm_add_action_or_reset", C) ||
      !ExprHasName(Registration->getArg(1),
                   "scmi_debugfs_common_cleanup", C))
    return;

  // A failing reset-registration call has already invoked its callback on the
  // registered object; an explicit cleanup of that same object frees it twice.
  if (!refersToSameObject(CleanupCall->getArg(0), Registration->getArg(2)))
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
