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
#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"

using namespace clang;
using namespace ento;
using namespace taint;

namespace {
class SAGenTestChecker
  : public Checker<
        check::PreCall
    > {
   mutable std::unique_ptr<BugType> BT;

   public:
      SAGenTestChecker()
          : BT(new BugType(this, "Unbounded roundup_pow_of_two() argument on 32-bit",
                                 "Integer overflow")) {}

      void checkPreCall(const CallEvent &Call, CheckerContext &C) const;

   private:
      // Helpers
      static bool isRoundupPow2Callee(const CallEvent &Call);
      static bool argMayExceedPow2Limit(const CallEvent &Call, CheckerContext &C);
      void reportIssue(const CallExpr *CE, CheckerContext &C) const;
};

// Helper: locate the rounding sink. roundup_pow_of_two() is a kernel macro
// that routes non-constant inputs to __roundup_pow_of_two(); either spelling
// denotes the same sink. The name only locates the sink: whether we warn is
// decided by the argument's value relation checked below.
bool SAGenTestChecker::isRoundupPow2Callee(const CallEvent &Call) {
  const FunctionDecl *FD = dyn_cast_or_null<FunctionDecl>(Call.getDecl());
  if (!FD || !FD->getIdentifier())
    return false;
  return FD->getName().contains("roundup_pow_of_two");
}

// Helper: on a 32-bit unsigned long target, rounding a value above
// 1UL << 31 makes the internal "1UL << fls(n - 1)" shift undefined. The fix
// installs exactly that bound as a path guard before the call, so consult
// the constraint manager: warn only while the argument can still exceed the
// bound on the current path (this survives renaming and unrelated inserts).
bool SAGenTestChecker::argMayExceedPow2Limit(const CallEvent &Call, CheckerContext &C) {
  if (Call.getNumArgs() < 1)
    return false;

  // Portable caller-side contract of roundup_pow_of_two(): the rounded
  // result must stay representable for the smallest unsigned long this code
  // can be built for (32 bits), which requires n <= 1UL << 31. The patch
  // installs exactly that value relation as a path guard, so evaluate the
  // bound in the argument's own type, independent of the analysis target's
  // word size.
  const uint64_t Bound = (uint64_t)1 << 31;

  SVal ArgV = Call.getArgSVal(0);

  // A fully concrete argument is compared directly against the bound.
  if (const auto CI = ArgV.getAs<nonloc::ConcreteInt>())
    return CI->getValue().getZExtValue() > Bound;

  SymbolRef Sym = ArgV.getAsSymbol();
  if (!Sym)
    return false;

  ASTContext &ACtx = C.getASTContext();
  ProgramStateRef State = C.getState();

  // The sink receives the loaded field (e.g. the unsigned 32-bit
  // max_entries counter), usually widened to unsigned long by an implicit
  // cast. Ask whether the argument can still exceed the bound for the
  // symbol as passed and, after stripping value-preserving casts, for the
  // underlying field symbol the patch guard constrains; warn only when
  // every view can still lie outside [0, 1UL << 31].
  auto MayExceedBound = [&](SymbolRef S) -> bool {
    QualType Ty = S->getType();
    if (!Ty->isIntegerType())
      return false;

    const unsigned Bits = ACtx.getTypeSize(Ty);
    const bool IsUnsigned = Ty->isUnsignedIntegerType();
    // Abstain when the argument's own value domain cannot exceed the bound
    // (narrow fields or signed 32-bit values).
    if (Bits < 32 || (!IsUnsigned && Bits <= 32))
      return false;

    const llvm::APSInt From(llvm::APInt(Bits, 0), IsUnsigned);
    const llvm::APSInt To(llvm::APInt(Bits, Bound), IsUnsigned);

    std::pair<ProgramStateRef, ProgramStateRef> States =
        State->getConstraintManager().assumeInclusiveRangeDual(
            State, nonloc::SymbolVal(S), From, To);
    // States.second is the successor where the argument lies outside
    // [0, 1UL << 31]; while that successor is feasible the rounding shift
    // overflows a 32-bit build of this code.
    return States.second != nullptr;
  };

  if (!MayExceedBound(Sym))
    return false;

  while (const auto *Cast = dyn_cast<SymbolCast>(Sym))
    Sym = Cast->getOperand();

  return MayExceedBound(Sym);
}

// Report a concise warning at the rounding call
void SAGenTestChecker::reportIssue(const CallExpr *CE, CheckerContext &C) const {
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto R = std::make_unique<PathSensitiveBugReport>(
      *BT,
      "roundup_pow_of_two() argument may exceed 1UL << 31 on this path; "
      "the rounding shift is undefined for 32-bit unsigned long",
      N);

  if (CE)
    R->addRange(CE->getSourceRange());

  C.emitReport(std::move(R));
}

// The rounding sink: the argument must already be constrained to at most
// 1UL << 31 at this point of the path, otherwise the shift performed inside
// the rounding is undefined behavior whenever this portable code is built
// for a 32-bit unsigned long. The patch's added guard (reject
// attr->max_entries > 1UL << 31) supplies exactly that constraint, so the
// fixed revision stays silent. The bound is a target-independent value
// relation, so it is checked in the argument's own type instead of gating
// on the analysis target's word size.
void SAGenTestChecker::checkPreCall(const CallEvent &Call, CheckerContext &C) const {
  if (!isRoundupPow2Callee(Call))
    return;

  if (!argMayExceedPow2Limit(Call, C))
    return;

  reportIssue(dyn_cast_or_null<CallExpr>(Call.getOriginExpr()), C);
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detect roundup_pow_of_two() arguments not bounded by 1UL << 31 on 32-bit targets",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
