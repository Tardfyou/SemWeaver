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
#include "clang/Lex/Lexer.h"

using namespace clang;
using namespace ento;
using namespace taint;

// No custom program state needed.

namespace {
class SAGenTestChecker : public Checker<check::PreCall> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Overflow-prone allocation size (use kcalloc)",
                       "API Misuse")) {}

  void checkPreCall(const CallEvent &Call, CheckerContext &C) const;

private:
  bool isArrayAwareAllocator(const CallEvent &Call,
                             CheckerContext &C) const;

  bool getAllocatorSizeArgIndex(const CallEvent &Call, unsigned &Idx,
                                CheckerContext &C) const;

  static bool exprContainsSizeof(const Expr *E);

  // Suppress a direct parameter count only when its entire type range,
  // multiplied by the constant sizeof operand, fits in size_t.
  static bool isProvablyNonOverflowingParameterCount(const Expr *Count,
                                                     const Expr *SizeofExpr,
                                                     CheckerContext &C);

  void reportMulPattern(const BinaryOperator *Mul, CheckerContext &C) const;
};

bool SAGenTestChecker::isArrayAwareAllocator(const CallEvent &Call,
                                             CheckerContext &C) const {
  const Expr *Orig = Call.getOriginExpr();
  if (!Orig)
    return false;

  static const char *ArrayAware[] = {
      "kcalloc", "kvcalloc", "kmalloc_array", "kvmalloc_array", "devm_kcalloc"};

  for (const char *Name : ArrayAware) {
    if (ExprHasName(Orig, Name, C))
      return true;
  }
  return false;
}

bool SAGenTestChecker::getAllocatorSizeArgIndex(const CallEvent &Call,
                                                unsigned &Idx,
                                                CheckerContext &C) const {
  const Expr *Orig = Call.getOriginExpr();
  if (!Orig)
    return false;

  struct Entry {
    const char *Name;
    unsigned SizeIdx;
  };

  // Keep more specific names before names that may be their substrings.
  static const Entry Targets[] = {
      {"devm_kzalloc", 1}, {"devm_kmalloc", 1}, {"kvzalloc", 0},
      {"kvmalloc", 0},     {"kzalloc", 0},      {"kmalloc", 0},
      {"vzalloc", 0}};

  for (const auto &E : Targets) {
    if (ExprHasName(Orig, E.Name, C)) {
      Idx = E.SizeIdx;
      return true;
    }
  }
  return false;
}

bool SAGenTestChecker::exprContainsSizeof(const Expr *E) {
  if (!E)
    return false;

  const auto *UETT =
      findSpecificTypeInChildren<UnaryExprOrTypeTraitExpr>(E);
  return UETT && UETT->getKind() == UETT_SizeOf;
}

bool SAGenTestChecker::isProvablyNonOverflowingParameterCount(
    const Expr *Count, const Expr *SizeofExpr, CheckerContext &C) {
  if (!Count || !SizeofExpr)
    return false;

  Count = Count->IgnoreParenImpCasts();
  const auto *DRE = dyn_cast<DeclRefExpr>(Count);
  const auto *Param = DRE ? dyn_cast<ParmVarDecl>(DRE->getDecl()) : nullptr;
  if (!Param)
    return false;

  QualType CountType = Param->getType();
  if (!CountType->isIntegerType() || CountType->isBooleanType())
    return false;

  Expr::EvalResult SizeResult;
  if (!SizeofExpr->EvaluateAsInt(SizeResult, C.getASTContext()))
    return false;

  const llvm::APSInt &EvaluatedSize = SizeResult.Val.getInt();
  if (EvaluatedSize.isSigned() && EvaluatedSize.isNegative())
    return false;

  const unsigned WideWidth = 128;
  const unsigned CountWidth = C.getASTContext().getTypeSize(CountType);
  if (CountWidth == 0 || CountWidth >= WideWidth)
    return false;

  llvm::APInt MaxCount =
      CountType->isUnsignedIntegerType()
          ? llvm::APInt::getMaxValue(CountWidth)
          : llvm::APInt::getSignedMaxValue(CountWidth);
  MaxCount = MaxCount.zext(WideWidth);

  llvm::APInt SizeValue = EvaluatedSize;
  if (SizeValue.getActiveBits() > WideWidth)
    return false;
  SizeValue = SizeValue.zextOrTrunc(WideWidth);

  llvm::APInt Product = MaxCount * SizeValue;
  const unsigned SizeTWidth =
      C.getASTContext().getTypeSize(C.getASTContext().getSizeType());
  if (SizeTWidth == 0 || SizeTWidth > WideWidth)
    return false;

  llvm::APInt MaxSizeT = llvm::APInt::getMaxValue(SizeTWidth).zext(WideWidth);
  return Product.ule(MaxSizeT);
}

void SAGenTestChecker::reportMulPattern(const BinaryOperator *Mul,
                                        CheckerContext &C) const {
  if (!Mul)
    return;

  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto R = std::make_unique<PathSensitiveBugReport>(
      *BT,
      "Use kcalloc(count, size, ...) instead of count*sizeof in allocation "
      "to avoid integer overflow",
      N);
  R->addRange(Mul->getSourceRange());
  C.emitReport(std::move(R));
}

void SAGenTestChecker::checkPreCall(const CallEvent &Call,
                                    CheckerContext &C) const {
  if (isArrayAwareAllocator(Call, C))
    return;

  unsigned SizeIdx = 0;
  if (!getAllocatorSizeArgIndex(Call, SizeIdx, C))
    return;

  if (Call.getNumArgs() <= SizeIdx)
    return;

  const Expr *SizeE = Call.getArgExpr(SizeIdx);
  if (!SizeE)
    return;

  if (ExprHasName(SizeE, "array_size", C) ||
      ExprHasName(SizeE, "struct_size", C) ||
      ExprHasName(SizeE, "flex_array_size", C))
    return;

  SizeE = SizeE->IgnoreParenImpCasts();
  const auto *BO = dyn_cast<BinaryOperator>(SizeE);
  if (!BO || BO->getOpcode() != BO_Mul)
    return;

  const Expr *LHS = BO->getLHS()->IgnoreParenImpCasts();
  const Expr *RHS = BO->getRHS()->IgnoreParenImpCasts();

  const bool LHSHasSizeof = exprContainsSizeof(LHS);
  const bool RHSHasSizeof = exprContainsSizeof(RHS);
  if (!LHSHasSizeof && !RHSHasSizeof)
    return;

  // The reported criu_checkpoint_devices case passes num_devices as a
  // uint32_t parameter. On 64-bit targets its full type range times this
  // constant element size fits in size_t, so this multiplication cannot
  // wrap. Do not apply this suppression to direct field expressions such as
  // args->num_of_nodes; those remain eligible for the target diagnostic.
  if (LHSHasSizeof &&
      isProvablyNonOverflowingParameterCount(RHS, LHS, C))
    return;
  if (RHSHasSizeof &&
      isProvablyNonOverflowingParameterCount(LHS, RHS, C))
    return;

  reportMulPattern(BO, C);
}

} // namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects kmalloc/kzalloc-style allocations that multiply count by "
      "sizeof; suggest kcalloc to avoid overflow",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
