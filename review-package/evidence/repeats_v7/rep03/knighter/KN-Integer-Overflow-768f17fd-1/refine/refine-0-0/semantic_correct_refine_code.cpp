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
#include "clang/Lex/Lexer.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/Support/raw_ostream.h"

#include <memory>

using namespace clang;
using namespace ento;

namespace {

class SAGenTestChecker : public Checker<check::PreStmt<BinaryOperator>> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(
            this, "Potential 32-bit left shift without 64-bit upcasting")) {}

  void checkPreStmt(const BinaryOperator *BOp, CheckerContext &C) const;

private:
  /// Returns true when Loc was expanded from MacroName, including nested
  /// macro-expansion frames.
  bool isExpandedFromMacroNamed(SourceLocation Loc, llvm::StringRef MacroName,
                                CheckerContext &C) const;

  /// FIELD_PREP and REG_FIELD_PREP intentionally construct a register image:
  ///
  ///   ((value << field_shift) & field_mask)
  ///
  /// The final mask is part of the field-packing contract. Such shifts are
  /// unrelated to the checker target, which is a value intended to become
  /// 64-bit only after an already-narrow shift has taken place.
  bool isIntentionalFieldPackingShift(const BinaryOperator *BOp,
                                      CheckerContext &C) const;

  /// Gets an exact integer value known through either AST constant evaluation
  /// or the current path-sensitive program state.
  bool getKnownIntegerValue(const Expr *E, llvm::APSInt &Value,
                            CheckerContext &C) const;

  /// Gets a symbolic upper bound from the ConstraintManager when available.
  bool getKnownMaximumValue(const Expr *E, llvm::APSInt &Value,
                            CheckerContext &C) const;

  /// For signed shifts, a non-negative operand is needed before a safe shift
  /// can be proven.
  bool isKnownNonNegative(const Expr *E, CheckerContext &C) const;

  /// Returns true unless the current analyzer state can prove that the shift
  /// does not overflow the effective type of the left operand.
  bool mayOverflow(const BinaryOperator *BOp, CheckerContext &C) const;
};

bool SAGenTestChecker::isExpandedFromMacroNamed(
    SourceLocation Loc, llvm::StringRef MacroName, CheckerContext &C) const {
  const SourceManager &SM = C.getSourceManager();
  const LangOptions &LangOpts = C.getLangOpts();

  while (Loc.isMacroID()) {
    if (Lexer::getImmediateMacroName(Loc, SM, LangOpts) == MacroName)
      return true;

    SourceLocation CallerLoc = SM.getImmediateMacroCallerLoc(Loc);
    if (CallerLoc.isInvalid() || CallerLoc == Loc)
      break;

    Loc = CallerLoc;
  }

  return false;
}

bool SAGenTestChecker::isIntentionalFieldPackingShift(
    const BinaryOperator *BOp, CheckerContext &C) const {
  const SourceLocation OpLoc = BOp->getOperatorLoc();

  // REG_FIELD_PREP commonly expands through FIELD_PREP, so recognize both
  // names in the macro expansion stack.
  if (!isExpandedFromMacroNamed(OpLoc, "REG_FIELD_PREP", C) &&
      !isExpandedFromMacroNamed(OpLoc, "FIELD_PREP", C))
    return false;

  // Require the shift to be structurally used as an operand of a bitwise AND.
  // This prevents suppressing unrelated shifts that merely happen to occur in
  // a field-preparation macro expansion.
  const Expr *Child = BOp;

  for (unsigned Depth = 0; Depth != 4; ++Depth) {
    auto Parents = C.getASTContext().getParents(*Child);
    if (Parents.empty())
      return false;

    const Expr *Parent = Parents[0].get<Expr>();
    if (!Parent)
      return false;

    if (const auto *ParentBO = dyn_cast<BinaryOperator>(Parent)) {
      if (ParentBO->getOpcode() != BO_And)
        return false;

      const Expr *ParentLHS = ParentBO->getLHS()->IgnoreParenImpCasts();
      const Expr *ParentRHS = ParentBO->getRHS()->IgnoreParenImpCasts();
      const Expr *ShiftExpr = Child->IgnoreParenImpCasts();

      return ParentLHS == ShiftExpr || ParentRHS == ShiftExpr;
    }

    // FIELD_PREP expansions can place parentheses or implicit casts between
    // the shift and the final mask operation.
    if (isa<ParenExpr>(Parent) || isa<ImplicitCastExpr>(Parent) ||
        isa<CStyleCastExpr>(Parent)) {
      Child = Parent;
      continue;
    }

    return false;
  }

  return false;
}

bool SAGenTestChecker::getKnownIntegerValue(const Expr *E,
                                            llvm::APSInt &Value,
                                            CheckerContext &C) const {
  if (!E)
    return false;

  Expr::EvalResult EvalResult;
  if (E->EvaluateAsInt(EvalResult, C.getASTContext())) {
    Value = EvalResult.Val.getInt();
    return true;
  }

  SVal ExprValue = C.getState()->getSVal(E, C.getLocationContext());
  if (auto ConcreteValue = ExprValue.getAs<nonloc::ConcreteInt>()) {
    Value = ConcreteValue->getValue();
    return true;
  }

  return false;
}

bool SAGenTestChecker::getKnownMaximumValue(const Expr *E,
                                             llvm::APSInt &Value,
                                             CheckerContext &C) const {
  if (getKnownIntegerValue(E, Value, C))
    return true;

  ProgramStateRef State = C.getState();
  SVal ExprValue = State->getSVal(E, C.getLocationContext());
  SymbolRef Sym = ExprValue.getAsSymbol();
  if (!Sym)
    return false;

  const llvm::APSInt *MaxValue =
      State->getConstraintManager().getSymMaxVal(State, Sym);
  if (!MaxValue)
    return false;

  Value = *MaxValue;
  return true;
}

bool SAGenTestChecker::isKnownNonNegative(const Expr *E,
                                           CheckerContext &C) const {
  llvm::APSInt ExactValue(64, 0);
  if (getKnownIntegerValue(E, ExactValue, C))
    return !ExactValue.isNegative();

  ProgramStateRef State = C.getState();
  SVal ExprValue = State->getSVal(E, C.getLocationContext());
  SymbolRef Sym = ExprValue.getAsSymbol();
  if (!Sym)
    return false;

  const llvm::APSInt *MinValue =
      State->getConstraintManager().getSymMinVal(State, Sym);
  return MinValue && !MinValue->isNegative();
}

bool SAGenTestChecker::mayOverflow(const BinaryOperator *BOp,
                                   CheckerContext &C) const {
  const Expr *LHS = BOp->getLHS();
  const Expr *RHS = BOp->getRHS();
  QualType LHSType = LHS->getType();

  const unsigned TypeWidth = C.getASTContext().getTypeSize(LHSType);

  llvm::APSInt ShiftAmount(64, 0);
  if (!getKnownIntegerValue(RHS, ShiftAmount, C))
    return true;

  // A negative shift count or a count at least as large as the promoted LHS
  // width is invalid and must remain reportable.
  if (ShiftAmount.isNegative())
    return true;

  const uint64_t Shift = ShiftAmount.getLimitedValue();
  if (Shift >= TypeWidth)
    return true;

  // A signed left shift is only safe after proving that the operand cannot be
  // negative. For unsigned values, the maximum bound is sufficient.
  if (LHSType->isSignedIntegerType() && !isKnownNonNegative(LHS, C))
    return true;

  llvm::APSInt LHSMax(64, 0);
  if (!getKnownMaximumValue(LHS, LHSMax, C))
    return true;

  if (LHSMax.isNegative())
    return true;

  llvm::APInt MaximumRepresentable =
      LHSType->isSignedIntegerType()
          ? llvm::APInt::getSignedMaxValue(TypeWidth)
          : llvm::APInt::getMaxValue(TypeWidth);

  const llvm::APInt SafeLHSMaximum = MaximumRepresentable.lshr(Shift);
  const llvm::APInt ObservedLHSMaximum = LHSMax.zextOrTrunc(TypeWidth);

  return ObservedLHSMaximum.ugt(SafeLHSMaximum);
}

void SAGenTestChecker::checkPreStmt(const BinaryOperator *BOp,
                                    CheckerContext &C) const {
  if (BOp->getOpcode() != BO_Shl)
    return;

  const Expr *LHS = BOp->getLHS();
  if (!LHS)
    return;

  // Check the effective type of the shifted expression itself. Do not search
  // its descendants for an arbitrary cast: a nested u64 cast does not prove
  // that the shift is actually evaluated as a 64-bit operation.
  QualType LHSType = LHS->getType();
  if (!LHSType->isIntegerType())
    return;

  const unsigned TypeWidth = C.getASTContext().getTypeSize(LHSType);
  if (TypeWidth >= 64)
    return;

  // REG_FIELD_PREP/FIELD_PREP build a masked hardware register value. The
  // reported i915 rxy shifts are exactly this case.
  if (isIntentionalFieldPackingShift(BOp, C))
    return;

  // Avoid reporting shifts whose count and operand range are proven safe by
  // the current path state. This handles harmless expressions such as
  // `1 << x_w` when x_w is known to be 2.
  if (!mayOverflow(BOp, C))
    return;

  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT,
      "Potential integer overflow: left shift performed on a sub-64-bit "
      "value without upcasting to 64-bit",
      N);
  Report->addRange(BOp->getSourceRange());
  C.emitReport(std::move(Report));
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &Registry) {
  Registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects arithmetic shifts on sub-64-bit integers without prior "
      "upcasting to 64-bit",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
