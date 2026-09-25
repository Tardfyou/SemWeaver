// /external/upstream-study/SAGEN/result-0224-bugfail-multi-o3mini/test-Integer-Overflow-768f17fd25e4a98bf5166148629ecf6f647d5efc/checkers/checker1.cpp
#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/StaticAnalyzer/Checkers/utility.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;
using namespace ento;

namespace {

// The checker only needs to hook the PreStmt callback for BinaryOperator.
class SAGenTestChecker : public Checker< check::PreStmt<BinaryOperator> > {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Potential 32-bit left shift without 64-bit upcasting")) {}

  void checkPreStmt(const BinaryOperator *BOp, CheckerContext &C) const;

};

void SAGenTestChecker::checkPreStmt(const BinaryOperator *BOp, CheckerContext &C) const {
  // We are only interested in left shift operators.
  if (BOp->getOpcode() != BO_Shl)
    return;

  const Expr *LHS = BOp->getLHS();
  if (!LHS)
    return;

  // The affected overflow is the final shift of a value composed from the
  // shifted field bit and field value; the inner bit shift is not that sink.
  const Expr *ComposedLHS = LHS->IgnoreParenImpCasts();
  const auto *Composition = dyn_cast<BinaryOperator>(ComposedLHS);
  if (!Composition || Composition->getOpcode() != BO_Or)
    return;

  QualType LHSType = LHS->getType();
  // Proceed only if the LHS is an integer type.
  if (!LHSType->isIntegerType())
    return;

  unsigned typeWidth = C.getASTContext().getTypeSize(LHSType);
  // If the width is less than 64 bits, then shifting might lead to overflow.
  if (typeWidth >= 64)
    return;

  // Report a bug if a 32-bit (or sub-64-bit) integer is left-shifted without upcasting.
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto report = std::make_unique<PathSensitiveBugReport>(
      *BT, "Potential integer overflow: left shift performed on a 32-bit value without upcasting to 64-bit", N);
  report->addRange(BOp->getSourceRange());
  C.emitReport(std::move(report));
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects arithmetic shifts on 32-bit integers without prior upcasting to 64-bit",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
