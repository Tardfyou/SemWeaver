// /external/upstream-study/SAGEN/result-0224-bugfail-multi-o3mini/test-Out-of-Bound-13e788deb7348cc88df34bed736c3b3b9927ea52/checkers/checker0.cpp
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtVisitor.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/StaticAnalyzer/Checkers/utility.h"
#include "clang/Lex/Lexer.h"  // For Lexer::getSourceText
#include <memory>

using namespace clang;
using namespace ento;
using namespace taint;

namespace {

class SAGenTestChecker : public Checker<check::BranchCondition> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Off-by-one array index boundary check", "Array Bounds")) {}

  // This callback inspects branch conditions for incorrect boundary checks.
  void checkBranchCondition(const Stmt *Condition, CheckerContext &C) const;

private:
  // Helper function to report the bug.
  void reportBug(const Stmt *Condition, CheckerContext &C) const;
};

void SAGenTestChecker::checkBranchCondition(const Stmt *Condition, CheckerContext &C) const {
  if (!Condition)
    return;

  const Expr *ConditionExpr = dyn_cast<Expr>(Condition);
  if (!ConditionExpr)
    return;

  const BinaryOperator *BOp =
      dyn_cast<BinaryOperator>(ConditionExpr->IgnoreParenImpCasts());
  if (!BOp || BOp->getOpcode() != BO_GT)
    return;

  // The guarded quantity is an index read from a trace-position array.
  const Expr *GuardedValue = BOp->getLHS()->IgnoreParenImpCasts();
  if (!isa<ArraySubscriptExpr>(GuardedValue))
    return;

  const Expr *Capacity = BOp->getRHS();
  if (!ExprHasName(Capacity, "RDS_MSG_RX_DGRAM_TRACE_MAX", C))
    return;

  // Equality must be rejected: this value later serves as an index whose
  // downstream use reserves no slot for the capacity value itself.
  reportBug(BOp, C);
}

void SAGenTestChecker::reportBug(const Stmt *Condition, CheckerContext &C) const {
  // Generate a non-fatal error node.
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  // Emit a concise bug report indicating the off-by-one error.
  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT, "Off-by-one error: incorrect array index boundary check", N);
  Report->addRange(Condition->getSourceRange());
  C.emitReport(std::move(Report));
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects off-by-one error in array index boundary check",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
