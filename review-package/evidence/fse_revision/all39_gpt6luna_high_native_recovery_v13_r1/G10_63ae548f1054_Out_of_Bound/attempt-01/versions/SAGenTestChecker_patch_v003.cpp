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
#include "clang/AST/ExprCXX.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Type.h"

using namespace clang;
using namespace ento;
using namespace taint;

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
      static bool getIndexSymbol(const Expr *IdxE, CheckerContext &C,
                                 SymbolRef &SymOut, llvm::APSInt &ConstValOut, bool &IsConst);
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

bool SAGenTestChecker::getIndexSymbol(const Expr *IdxE, CheckerContext &C,
                                      SymbolRef &SymOut, llvm::APSInt &ConstValOut, bool &IsConst) {
  if (!IdxE)
    return false;

  // Check constant index
  if (EvaluateExprToInt(ConstValOut, IdxE, C)) {
    IsConst = true;
    return true;
  }

  // Otherwise, extract symbolic index
  ProgramStateRef State = C.getState();
  SVal V = State->getSVal(IdxE, C.getLocationContext());
  if (SymbolRef S = V.getAsSymbol()) {
    SymOut = S;
    IsConst = false;
    return true;
  }

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
  // Branch assumptions constrain the analyzer state; do not mark an index
  // checked here because this callback runs before either successor is known.
}

void SAGenTestChecker::checkPreStmt(const ArraySubscriptExpr *ASE, CheckerContext &C) const {
  if (!ASE)
    return;

  if (!isTFPtsArrayAccess(ASE, C))
    return;

  llvm::APInt ArrSize;
  if (!getArrayCapacityFromBase(ASE, ArrSize))
    return; // Cannot determine capacity, be conservative and skip

  const Expr *IdxE = ASE->getIdx();
  if (!IdxE)
    return;

  IdxE = IdxE->IgnoreParenCasts();

  // Case 1: constant index
  llvm::APSInt CVal;
  SymbolRef Sym{};
  bool IsConst = false;
  if (!getIndexSymbol(IdxE, C, Sym, CVal, IsConst))
    return;

  if (IsConst) {
    bool Neg = CVal.isSigned() && CVal.isNegative();
    uint64_t IdxVal = CVal.getLimitedValue(UINT64_MAX);
    uint64_t Cap = ArrSize.getLimitedValue(UINT64_MAX);
    if (Neg || IdxVal >= Cap) {
      reportAtASE(ASE, C, "Index out of bounds on LUT access");
    }
    return;
  }

  // Case 2: symbolic index
  ProgramStateRef State = C.getState();
  if (!Sym)
    return;

  // A path guard proves safety only when it constrains this index against
  // the capacity of the array being accessed.
  SVal IndexVal = State->getSVal(IdxE, C.getLocationContext());
  QualType IndexTy = IdxE->getType();
  SVal CapacityVal = C.getSValBuilder().makeIntVal(
      ArrSize.getLimitedValue(UINT64_MAX), IndexTy);
  SVal ZeroVal = C.getSValBuilder().makeZeroVal(IndexTy);
  SVal BelowCapacity = C.getSValBuilder().evalBinOp(
      State, BO_LT, IndexVal, CapacityVal, C.getASTContext().BoolTy);
  SVal NonNegative = C.getSValBuilder().evalBinOp(
      State, BO_GE, IndexVal, ZeroVal, C.getASTContext().BoolTy);
  auto BelowCapacityCond = BelowCapacity.getAs<DefinedOrUnknownSVal>();
  auto NonNegativeCond = NonNegative.getAs<DefinedOrUnknownSVal>();
  if (BelowCapacityCond && NonNegativeCond &&
      !State->assume(*BelowCapacityCond, false) &&
      !State->assume(*NonNegativeCond, false))
    return;

  // Try to infer an upper bound
  const llvm::APSInt *Max = inferSymbolMaxVal(Sym, C);
  if (Max) {
    uint64_t MaxV = Max->getLimitedValue(UINT64_MAX);
    uint64_t Cap = ArrSize.getLimitedValue(UINT64_MAX);
    if (MaxV >= Cap) {
      reportAtASE(ASE, C, "Possible OOB: index may exceed TRANSFER_FUNC_POINTS");
      return;
    }
    // If MaxV < Cap, likely safe; do not warn.
    return;
  }

  // If no guard seen and no bound known, report missing validation
  reportAtASE(ASE, C, "Possible OOB: index not validated against TRANSFER_FUNC_POINTS");
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
