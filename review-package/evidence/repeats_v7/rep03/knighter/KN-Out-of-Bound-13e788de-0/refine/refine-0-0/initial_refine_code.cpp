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
#include "clang/Lex/Lexer.h"  // For Lexer::getSourceText
#include <memory>

using namespace clang;
using namespace ento;
using namespace taint;

namespace {

REGISTER_SET_WITH_PROGRAMSTATE(ReportedComparisons, const Stmt *)

/// Returns true only when the expression is an access to a statically-sized
/// array and the supplied bound is exactly that array's element count.
static bool isArrayElementComparedWithItsCapacity(
    const BinaryOperator *BOp, CheckerContext &C) {
  if (!BOp || BOp->getOpcode() != BO_GT)
    return false;

  const Expr *RHS = BOp->getRHS();
  if (!RHS)
    return false;

  // Keep this checker scoped to the RDS latency trace boundary check.
  if (!ExprHasName(RHS, "RDS_MSG_RX_DGRAM_TRACE_MAX", C))
    return false;

  const Expr *LHS = BOp->getLHS();
  if (!LHS)
    return false;

  // A count field such as trace.rx_traces is not an array index. Requiring an
  // actual array-element access excludes the reported false positive.
  const auto *ArrayAccess = dyn_cast<ArraySubscriptExpr>(
      LHS->IgnoreParenImpCasts());
  if (!ArrayAccess)
    return false;

  const Expr *Base = ArrayAccess->getBase();
  if (!Base)
    return false;

  // Ignore the implicit array-to-pointer decay introduced by subscripting.
  Base = Base->IgnoreParenImpCasts();

  const Type *BaseType =
      Base->getType().getCanonicalType().getTypePtr();
  const auto *ArrayType = dyn_cast<ConstantArrayType>(BaseType);
  if (!ArrayType)
    return false;

  llvm::APSInt Boundary;
  if (!EvaluateExprToInt(Boundary, RHS, C) || Boundary.isNegative())
    return false;

  const llvm::APInt &ArraySize = ArrayType->getSize();

  // Compare APInts at a common width, because a macro expansion may have a
  // narrower integer type than the array-size APInt.
  unsigned CompareWidth = Boundary.getBitWidth();
  if (ArraySize.getBitWidth() > CompareWidth)
    CompareWidth = ArraySize.getBitWidth();

  const llvm::APInt BoundaryValue =
      Boundary.getValue().zextOrTrunc(CompareWidth);
  const llvm::APInt ArraySizeValue =
      ArraySize.zextOrTrunc(CompareWidth);

  return BoundaryValue == ArraySizeValue;
}

class ArrayIndexBoundaryVisitor
    : public RecursiveASTVisitor<ArrayIndexBoundaryVisitor> {
  CheckerContext &C;
  const BinaryOperator *Candidate = nullptr;

public:
  explicit ArrayIndexBoundaryVisitor(CheckerContext &C) : C(C) {}

  bool VisitBinaryOperator(BinaryOperator *BOp) {
    if (Candidate)
      return false;

    if (isArrayElementComparedWithItsCapacity(BOp, C)) {
      Candidate = BOp;
      return false;
    }

    return true;
  }

  const BinaryOperator *getCandidate() const {
    return Candidate;
  }
};

class SAGenTestChecker : public Checker<check::BranchCondition> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Off-by-one array index boundary check",
                       "Array Bounds")) {}

  void checkBranchCondition(const Stmt *Condition,
                            CheckerContext &C) const;

private:
  void reportBug(const BinaryOperator *BOp, CheckerContext &C) const;
};

void SAGenTestChecker::checkBranchCondition(
    const Stmt *Condition, CheckerContext &C) const {
  if (!Condition)
    return;

  ArrayIndexBoundaryVisitor Visitor(C);
  Visitor.TraverseStmt(const_cast<Stmt *>(Condition));

  const BinaryOperator *BOp = Visitor.getCandidate();
  if (!BOp)
    return;

  reportBug(BOp, C);
}

void SAGenTestChecker::reportBug(
    const BinaryOperator *BOp, CheckerContext &C) const {
  ProgramStateRef State = C.getState();

  // A branch condition may be revisited along different analyzer paths,
  // particularly in loops. Report each source comparison only once.
  if (State->get<ReportedComparisons>().contains(BOp))
    return;

  State = State->add<ReportedComparisons>(BOp);

  ExplodedNode *N = C.generateNonFatalErrorNode(State);
  if (!N)
    return;

  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT, "Off-by-one error: incorrect array index boundary check", N);
  Report->addRange(BOp->getSourceRange());
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
