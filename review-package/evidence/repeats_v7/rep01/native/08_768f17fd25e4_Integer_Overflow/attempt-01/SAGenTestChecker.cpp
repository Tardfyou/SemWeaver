// /external/upstream-study/SAGEN/result-0224-bugfail-multi-o3mini/test-Integer-Overflow-768f17fd25e4a98bf5166148629ecf6f647d5efc/checkers/checker1.cpp
#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
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
  bool isAtLeast64BitInteger(const Expr *E, CheckerContext &C) const;
};

bool SAGenTestChecker::isAtLeast64BitInteger(const Expr *E,
                                                CheckerContext &C) const {
  QualType Ty = E->getType();
  return Ty->isIntegerType() &&
         C.getASTContext().getTypeSize(Ty) >= 64;
}

class NestedLeftShiftVisitor
    : public RecursiveASTVisitor<NestedLeftShiftVisitor> {
public:
  bool Found = false;

  bool VisitBinaryOperator(BinaryOperator *BOp) {
    Found = Found || BOp->getOpcode() == BO_Shl;
    return !Found;
  }
};

static bool hasNestedLeftShift(const Expr *E) {
  NestedLeftShiftVisitor Visitor;
  Visitor.TraverseStmt(const_cast<Expr *>(E));
  return Visitor.Found;
}

void SAGenTestChecker::checkPreStmt(const BinaryOperator *BOp, CheckerContext &C) const {
  // We are only interested in left shift operators.
  if (BOp->getOpcode() != BO_Shl)
    return;

  const Expr *LHS = BOp->getLHS();
  if (!LHS)
    return;

  // The shift is safe when its operand has already entered a 64-bit domain.
  if (isAtLeast64BitInteger(LHS, C))
    return;

  QualType LHSType = LHS->getType();
  if (!LHSType->isIntegerType())
    return;

  unsigned typeWidth = C.getASTContext().getTypeSize(LHSType);
  if (typeWidth >= 64)
    return;

  // Target the final scaling shift of a composed bitfield, rather than its
  // bounded component shift.
  if (!hasNestedLeftShift(LHS))
    return;

  // Report a bug if a composite 32-bit value is left-shifted without widening.
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
