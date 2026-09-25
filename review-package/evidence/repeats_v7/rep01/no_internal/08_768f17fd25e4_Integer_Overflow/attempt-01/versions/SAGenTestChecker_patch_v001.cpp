// /external/upstream-study/SAGEN/result-0224-bugfail-multi-o3mini/test-Integer-Overflow-768f17fd25e4a98bf5166148629ecf6f647d5efc/checkers/checker1.cpp
#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/Stmt.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"

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

private:
  bool isResultImplicitlyWidenedTo64(const BinaryOperator *BOp,
                                     CheckerContext &C) const;
};

bool SAGenTestChecker::isResultImplicitlyWidenedTo64(
    const BinaryOperator *BOp, CheckerContext &C) const {
  const Stmt *Current = BOp;
  while (true) {
    auto Parents = C.getASTContext().getParents(*Current);
    if (Parents.size() != 1)
      return false;

    if (const auto *PE = Parents[0].get<ParenExpr>()) {
      Current = PE;
      continue;
    }

    const auto *CE = Parents[0].get<ImplicitCastExpr>();
    if (!CE)
      return false;

    QualType DestTy = CE->getType();
    return DestTy->isIntegerType() &&
           C.getASTContext().getTypeSize(DestTy) >= 64;
  }
}

void SAGenTestChecker::checkPreStmt(const BinaryOperator *BOp, CheckerContext &C) const {
  // We are only interested in left shift operators.
  if (BOp->getOpcode() != BO_Shl)
    return;

  const Expr *LHS = BOp->getLHS();
  if (!LHS)
    return;

  QualType LHSType = LHS->getType();
  // Proceed only if the LHS is an integer type.
  if (!LHSType->isIntegerType())
    return;

  unsigned typeWidth = C.getASTContext().getTypeSize(LHSType);
  // The motivating defect occurs when a narrow shift is widened only after
  // evaluation, so require that direct implicit conversion at its consumer.
  if (typeWidth >= 64 || !isResultImplicitlyWidenedTo64(BOp, C))
    return;

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
