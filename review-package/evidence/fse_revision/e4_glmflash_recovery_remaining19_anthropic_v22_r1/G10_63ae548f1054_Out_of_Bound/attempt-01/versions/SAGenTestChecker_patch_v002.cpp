#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Checkers/Taint.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/Environment.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/MemRegion.h"
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
#include "clang/AST/ExprCXX.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Type.h"

using namespace clang;
using namespace ento;
using namespace taint;

// For each index-variable region: the comparison operator and constant bound
// of a capacity guard evaluated on the current path, e.g.
// "if (i >= TRANSFER_FUNC_POINTS) return false;" before a LUT read.
REGISTER_MAP_WITH_PROGRAMSTATE(IdxCapacityGuardMap, const MemRegion *, const BinaryOperator *)

namespace {

class SAGenTestChecker
  : public Checker<
        check::PreStmt<ArraySubscriptExpr>,
        check::BranchCondition> {

   mutable std::unique_ptr<BugType> BT;

   public:
      SAGenTestChecker() : BT(new BugType(this, "Out-of-bounds LUT access", "Array bounds")) {}

      void checkPreStmt(const ArraySubscriptExpr *ASE, CheckerContext &C) const;
      void checkBranchCondition(const Stmt *Condition, CheckerContext &C) const;

   private:

      // Helpers
      static bool isTFPtsArrayAccess(const ArraySubscriptExpr *ASE, CheckerContext &C);
      static bool getArrayCapacityFromBase(const ArraySubscriptExpr *ASE, llvm::APInt &SizeOut);
      static bool isInsideLoop(const Stmt *S, CheckerContext &C);
      void reportAtASE(const ArraySubscriptExpr *ASE, CheckerContext &C, StringRef Msg) const;
};

bool SAGenTestChecker::isTFPtsArrayAccess(const ArraySubscriptExpr *ASE, CheckerContext &C) {
  if (!ASE)
    return false;

  const Expr *Base = ASE->getBase();
  if (!Base)
    return false;

  // Heuristic textual check to match: output_tf->tf_pts.{red,green,blue}
  if (!ExprHasName(Base, "tf_pts", C))
    return false;

  if (ExprHasName(Base, "red", C) || ExprHasName(Base, "green", C) || ExprHasName(Base, "blue", C))
    return true;

  return false;
}

bool SAGenTestChecker::getArrayCapacityFromBase(const ArraySubscriptExpr *ASE, llvm::APInt &SizeOut) {
  if (!ASE)
    return false;

  const Expr *Base = ASE->getBase();
  if (!Base)
    return false;

  const Expr *B = Base->IgnoreParenCasts();

  if (const auto *ME = dyn_cast<MemberExpr>(B)) {
    if (const auto *FD = dyn_cast<FieldDecl>(ME->getMemberDecl())) {
      QualType Ty = FD->getType();
      if (const auto *CAT = dyn_cast<ConstantArrayType>(Ty.getTypePtr())) {
        SizeOut = CAT->getSize();
        return true;
      }
    }
  }

  // Fallback: if the base is a DeclRefExpr of an array
  if (getArraySizeFromExpr(SizeOut, B))
    return true;

  return false;
}

bool SAGenTestChecker::isInsideLoop(const Stmt *S, CheckerContext &C) {
  if (!S)
    return false;
  if (findSpecificTypeInParents<ForStmt>(S, C) ||
      findSpecificTypeInParents<WhileStmt>(S, C) ||
      findSpecificTypeInParents<DoStmt>(S, C))
    return true;
  return false;
}

void SAGenTestChecker::reportAtASE(const ArraySubscriptExpr *ASE, CheckerContext &C, StringRef Msg) const {
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto R = std::make_unique<PathSensitiveBugReport>(*BT, Msg, N);
  R->addRange(ASE->getSourceRange());
  C.emitReport(std::move(R));
}

void SAGenTestChecker::checkBranchCondition(const Stmt *Condition, CheckerContext &C) const {
  ProgramStateRef State = C.getState();

  const Expr *CondE = dyn_cast_or_null<Expr>(Condition);
  if (!CondE) {
    C.addTransition(State);
    return;
  }

  // Record the capacity-guard relation for this path: a comparison between a
  // plain index variable and a compile-time-constant bound, e.g.
  // "if (i >= TRANSFER_FUNC_POINTS) return false;".  The bound is matched
  // numerically against the LUT capacity at the read itself, so the relation
  // survives renames of the macro, the variable, or the enclosing function.
  const auto *BO = dyn_cast<BinaryOperator>(CondE->IgnoreParenCasts());
  if (!BO || !BO->isComparisonOp()) {
    C.addTransition(State);
    return;
  }

  const Expr *LHS = BO->getLHS()->IgnoreParenCasts();
  const Expr *RHS = BO->getRHS()->IgnoreParenCasts();

  const Expr *IdxSide = nullptr;
  const Expr *ConstSide = nullptr;
  if (isa<DeclRefExpr>(LHS) && !isa<DeclRefExpr>(RHS)) {
    IdxSide = LHS;
    ConstSide = RHS;
  } else if (isa<DeclRefExpr>(RHS) && !isa<DeclRefExpr>(LHS)) {
    IdxSide = RHS;
    ConstSide = LHS;
  }

  if (!IdxSide || !ConstSide) {
    C.addTransition(State);
    return;
  }

  const auto *IdxDRE = dyn_cast<DeclRefExpr>(IdxSide);
  if (!IdxDRE || !isa<VarDecl>(IdxDRE->getDecl())) {
    C.addTransition(State);
    return;
  }

  llvm::APSInt BoundVal;
  if (!EvaluateExprToInt(BoundVal, ConstSide, C)) {
    C.addTransition(State);
    return;
  }

  const MemRegion *IdxReg = State->getSVal(IdxSide, C.getLocationContext()).getAsRegion();
  if (!IdxReg) {
    C.addTransition(State);
    return;
  }

  State = State->set<IdxCapacityGuardMap>(IdxReg, BO);
  C.addTransition(State);
}

void SAGenTestChecker::checkPreStmt(const ArraySubscriptExpr *ASE, CheckerContext &C) const {
  if (!ASE)
    return;

  if (!isTFPtsArrayAccess(ASE, C))
    return;

  llvm::APInt ArrSize;
  if (!getArrayCapacityFromBase(ASE, ArrSize))
    return; // Capacity unknown: nothing to relate the index to.

  const Expr *IdxE = ASE->getIdx();
  if (!IdxE)
    return;

  IdxE = IdxE->IgnoreParenCasts();

  // Case 1: compile-time constant index: decide by value.
  llvm::APSInt CVal;
  if (EvaluateExprToInt(CVal, IdxE, C)) {
    bool Neg = CVal.isSigned() && CVal.isNegative();
    uint64_t IdxVal = CVal.getLimitedValue(UINT64_MAX);
    uint64_t Cap = ArrSize.getLimitedValue(UINT64_MAX);
    if (Neg || IdxVal >= Cap)
      reportAtASE(ASE, C, "Index out of bounds on LUT access");
    return;
  }

  // The translated-curve pattern this checker models reads the LUT with a
  // loop index; standalone reads are out of scope.
  if (!isInsideLoop(ASE, C))
    return;

  const auto *DRE = dyn_cast<DeclRefExpr>(IdxE);
  if (!DRE || !isa<VarDecl>(DRE->getDecl()))
    return; // Not a plain variable index; no relation to establish.

  ProgramStateRef State = C.getState();
  SVal IdxV = State->getSVal(DRE, C.getLocationContext());
  const MemRegion *IdxReg = IdxV.getAsRegion();
  if (!IdxReg)
    return;

  // The read is safe only when this path related the index variable to the
  // array capacity: checkBranchCondition recorded the comparison and its
  // constant bound matches the capacity numerically ("i >= Cap" or "i < Cap"
  // keeps the index below Cap; "i > Cap - 1" / "i <= Cap - 1" likewise).
  bool Bounded = false;
  const BinaryOperator *const *GuardP = State->get<IdxCapacityGuardMap>(IdxReg);
  if (GuardP && *GuardP) {
    const BinaryOperator *G = *GuardP;
    const Expr *GL = G->getLHS()->IgnoreParenCasts();
    const Expr *GR = G->getRHS()->IgnoreParenCasts();
    const Expr *GConst = nullptr;
    if (isa<DeclRefExpr>(GL) && !isa<DeclRefExpr>(GR))
      GConst = GR;
    else if (isa<DeclRefExpr>(GR) && !isa<DeclRefExpr>(GL))
      GConst = GL;

    llvm::APSInt Bound;
    if (GConst && EvaluateExprToInt(Bound, GConst, C)) {
      uint64_t BoundV = Bound.getLimitedValue(UINT64_MAX);
      uint64_t Cap = ArrSize.getLimitedValue(UINT64_MAX);
      if (G->getOpcode() == BO_GE || G->getOpcode() == BO_LT)
        Bounded = (BoundV == Cap);
      else if (G->getOpcode() == BO_GT || G->getOpcode() == BO_LE)
        Bounded = (Cap >= 1 && BoundV == Cap - 1);
    }
  }

  // A path that provably keeps the index value below the capacity is safe as
  // well (e.g. a loop bound derived from a clamped point count).
  if (!Bounded) {
    if (SymbolRef Sym = IdxV.getAsSymbol()) {
      const llvm::APSInt *Max = inferSymbolMaxVal(Sym, C);
      if (Max) {
        bool Neg = Max->isSigned() && Max->isNegative();
        uint64_t MaxV = Max->getLimitedValue(UINT64_MAX);
        uint64_t Cap = ArrSize.getLimitedValue(UINT64_MAX);
        if (!Neg && MaxV < Cap)
          Bounded = true;
      }
    }
  }

  if (Bounded)
    return; // Index proved against the LUT capacity on this path.

  reportAtASE(ASE, C, "Loop index into LUT is not guarded against the array capacity");
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects missing TRANSFER_FUNC_POINTS bound checks for output_tf->tf_pts LUT accesses",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
