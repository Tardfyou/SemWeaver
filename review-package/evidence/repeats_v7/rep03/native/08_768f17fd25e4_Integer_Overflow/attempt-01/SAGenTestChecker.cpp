// /external/upstream-study/SAGEN/result-0224-bugfail-multi-o3mini/test-Integer-Overflow-768f17fd25e4a98bf5166148629ecf6f647d5efc/checkers/checker1.cpp
#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/Stmt.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramState.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SymExpr.h"
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

private:
  // A conversion after a narrow shift cannot protect the intermediate result.
  bool isWidenedAfterShift(const BinaryOperator *BOp, CheckerContext &C) const;
};

bool SAGenTestChecker::isWidenedAfterShift(const BinaryOperator *BOp,
                                           CheckerContext &C) const {
  const Expr *Current = BOp;

  while (true) {
    auto Parents = C.getASTContext().getParents(*Current);
    if (Parents.size() != 1)
      return false;

    if (const auto *CE = Parents[0].get<CastExpr>()) {
      QualType DestTy = CE->getType();
      return DestTy->isIntegerType() &&
             C.getASTContext().getTypeSize(DestTy) >= 64;
    }

    const auto *ParentExpr = Parents[0].get<ParenExpr>();
    if (!ParentExpr)
      return false;
    Current = ParentExpr;
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
  // If the width is less than 64 bits, then shifting might lead to overflow.
  if (typeWidth >= 64)
    return;

  // Report only when the narrow result is widened after this shift.
  if (!isWidenedAfterShift(BOp, C))
    return;

  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto report = std::make_unique<PathSensitiveBugReport>(
      *BT, "Potential integer overflow: narrow left-shift result widened only after the shift", N);
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
