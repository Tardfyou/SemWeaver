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

// Inclusive upper bound proven for a symbolic value along the current path.
// A guard such as `if (x > 1UL << 31) return ERR_PTR(-E2BIG);` proves
// x <= 2^31 on the fall-through path (the patch's capacity guard).
REGISTER_MAP_WITH_PROGRAMSTATE(Pow2UpperBoundMap, SymbolRef, uint64_t)

namespace {
class SAGenTestChecker
  : public Checker<
        check::PreCall,
        eval::Assume
    > {
   mutable std::unique_ptr<BugType> BT;

   public:
      SAGenTestChecker()
          : BT(new BugType(this, "Unbounded input to round-up-to-power-of-two",
                                 "Integer overflow")) {}

      void checkPreCall(const CallEvent &Call, CheckerContext &C) const;
      ProgramStateRef evalAssume(ProgramStateRef State, SVal Cond,
                                 bool Assumption) const;

   private:
      // Helpers
      static bool isRoundupPow2Callee(const CallEvent &Call);
      void reportUnboundedInput(const CallEvent &Call, const Expr *ArgE,
                                CheckerContext &C) const;
};

// The round-up-to-power-of-two operation: the kernel header implements
// roundup_pow_of_two() as a macro whose runtime branch calls the helper
// __roundup_pow_of_two(), so after preprocessing both spellings denote the
// same operation the analyzer can observe at a call site.
bool SAGenTestChecker::isRoundupPow2Callee(const CallEvent &Call) {
  const FunctionDecl *FD = dyn_cast_or_null<FunctionDecl>(Call.getDecl());
  if (!FD)
    return false;
  const IdentifierInfo *II = FD->getIdentifier();
  if (!II)
    return false;
  StringRef Name = II->getName();
  return Name == "roundup_pow_of_two" || Name == "__roundup_pow_of_two";
}

// Record path-proven upper bounds.  Whenever an assumption refutes a
// greater-than claim or asserts a less-or-equal claim against a non-negative
// constant, the symbol is provably <= that constant on this path.  This is
// how the patch's guard `if (attr->max_entries > 1UL << 31) return ...`
// becomes visible: on the fall-through path the input is bounded.
ProgramStateRef SAGenTestChecker::evalAssume(ProgramStateRef State,
                                             SVal Cond,
                                             bool Assumption) const {
  SymbolRef CondSym = Cond.getAsSymbol();
  if (!CondSym)
    return State;

  const SymExpr *Key = nullptr;
  const llvm::APSInt *RHS = nullptr;
  BinaryOperatorKind Rel = BO_Comma;

  if (const auto *SIE = dyn_cast<SymIntExpr>(CondSym)) {
    Key = SIE->getLHS();
    RHS = &SIE->getRHS();
    Rel = SIE->getOpcode();
  } else if (const auto *ISE = dyn_cast<IntSymExpr>(CondSym)) {
    // `RHS <op> Key` states the same claim as `Key <flipped op> RHS`.
    Key = ISE->getRHS();
    RHS = &ISE->getLHS();
    switch (ISE->getOpcode()) {
    case BO_LT: Rel = BO_GT; break;
    case BO_LE: Rel = BO_GE; break;
    case BO_GT: Rel = BO_LT; break;
    case BO_GE: Rel = BO_LE; break;
    default: return State;
    }
  } else {
    return State;
  }

  // An inclusive upper bound `Key <= Bound` is only proven when a
  // greater-than claim is refuted or a less-or-equal claim is asserted.
  bool HaveBound = false;
  uint64_t Bound = 0;
  switch (Rel) {
  case BO_GT:
    if (!Assumption) {
      HaveBound = true;
      Bound = RHS->getLimitedValue();
    }
    break;
  case BO_GE:
    if (!Assumption && !RHS->isZero()) {
      HaveBound = true;
      Bound = RHS->getLimitedValue() - 1;
    }
    break;
  case BO_LT:
    if (Assumption && !RHS->isZero()) {
      HaveBound = true;
      Bound = RHS->getLimitedValue() - 1;
    }
    break;
  case BO_LE:
    if (Assumption) {
      HaveBound = true;
      Bound = RHS->getLimitedValue();
    }
    break;
  default:
    return State;
  }

  // Only storable bounds (non-negative, fitting in 64 bits) are recorded.
  if (!HaveBound || RHS->isNegative() || RHS->getActiveBits() > 64)
    return State;

  if (const uint64_t *Old = State->get<Pow2UpperBoundMap>(Key))
    if (*Old < Bound)
      Bound = *Old;
  return State->set<Pow2UpperBoundMap>(Key, Bound);
}

// Warn when the value fed to the round-up-to-power-of-two operation carries
// no proven capacity bound on this path.  For a W-bit unsigned input, values
// above 2^(W-1) round up to 2^W, which is not representable in a W-bit
// `unsigned long` (32-bit targets): the computation overflows before any
// post-call zero check can run.  The patch fixes this by bounding the input
// before the call; a guard earlier on the path (recorded via evalAssume)
// keeps the patched revision silent.
void SAGenTestChecker::checkPreCall(const CallEvent &Call,
                                    CheckerContext &C) const {
  if (!isRoundupPow2Callee(Call) || Call.getNumArgs() < 1)
    return;

  const Expr *ArgE = Call.getArgExpr(0);
  if (!ArgE)
    return;

  // Capacity limit derived from the input's own type width, not from a
  // literal constant copied from the patch.
  QualType ValTy = ArgE->IgnoreParenImpCasts()->getType();
  if (!ValTy->isUnsignedIntegerType())
    return;
  uint64_t W = C.getASTContext().getTypeSize(ValTy);
  if (W < 2 || W > 64)
    return;
  uint64_t Threshold = (W == 64) ? (1ULL << 63) : (1ULL << (W - 1));

  SVal Arg = Call.getArgSVal(0);

  // Concrete inputs decide the overflow immediately.
  if (auto CI = Arg.getAs<nonloc::ConcreteInt>()) {
    if (CI->getValue().getActiveBits() <= 64 &&
        CI->getValue().getLimitedValue() > Threshold)
      reportUnboundedInput(Call, ArgE, C);
    return;
  }

  SymbolRef Sym = Arg.getAsSymbol();
  if (!Sym)
    return; // no symbolic value to bind the guard relation to

  // The guard must have bounded the argument symbol (or the same value seen
  // through a widening cast, u32 -> unsigned long) to at most the threshold.
  ProgramStateRef State = C.getState();
  const SymExpr *Cur = Sym;
  while (true) {
    if (const uint64_t *B = State->get<Pow2UpperBoundMap>(Cur))
      if (*B <= Threshold)
        return; // input bounded before rounding: patched behavior
    const auto *SC = dyn_cast<SymbolCast>(Cur);
    if (!SC)
      break;
    Cur = SC->getOperand();
  }

  reportUnboundedInput(Call, ArgE, C);
}

// Bind the report to the real argument expression and the rounding call.
void SAGenTestChecker::reportUnboundedInput(const CallEvent &Call,
                                            const Expr *ArgE,
                                            CheckerContext &C) const {
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto R = std::make_unique<PathSensitiveBugReport>(
      *BT,
      "Input to roundup_pow_of_two() has no upper bound on this path and may "
      "exceed the largest representable power of two of its type, so the "
      "rounded-up result can overflow unsigned long before any post-call "
      "check; bound the input before the call",
      N);
  R->addRange(ArgE->getSourceRange());
  if (const Expr *Origin = Call.getOriginExpr()) {
    PathDiagnosticLocation Loc =
        PathDiagnosticLocation::createBegin(Origin, C.getSourceManager(),
                                           C.getLocationContext());
    R->addNote("round-up-to-power-of-two applied to the unbounded input here",
               Loc);
  }
  C.emitReport(std::move(R));
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detect roundup_pow_of_two() applied to an unsigned input not bounded to the largest representable power of two of its type",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
