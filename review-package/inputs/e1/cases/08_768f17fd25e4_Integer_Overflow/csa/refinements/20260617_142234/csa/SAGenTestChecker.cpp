// /external/upstream-study/SAGEN/result-0224-bugfail-multi-o3mini/test-Integer-Overflow-768f17fd25e4a98bf5166148629ecf6f647d5efc/checkers/checker1.cpp
#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/AST/Decl.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SymExpr.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/StaticAnalyzer/Checkers/utility.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;
using namespace ento;

namespace {

// The checker hooks left shifts and reports only when the narrow shift result
// is later widened to a 64-bit quantity, which means the widening happens too late.
class SAGenTestChecker : public Checker< check::PreStmt<BinaryOperator> > {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Narrow left shift widened after evaluation")) {}

  void checkPreStmt(const BinaryOperator *BOp, CheckerContext &C) const;

private:
  bool isIntegerAtLeast64(QualType Ty, ASTContext &ACtx) const;
  bool isWidenedBeforeShift(const Expr *E, CheckerContext &C) const;
  bool isShiftResultUsedAs64(const BinaryOperator *BOp, CheckerContext &C) const;
};

bool SAGenTestChecker::isIntegerAtLeast64(QualType Ty, ASTContext &ACtx) const {
  if (Ty.isNull() || !Ty->isIntegerType())
    return false;
  return ACtx.getTypeSize(Ty) >= 64;
}

bool SAGenTestChecker::isWidenedBeforeShift(const Expr *E, CheckerContext &C) const {
  E = E ? E->IgnoreParens() : nullptr;
  if (!E)
    return false;

  if (const auto *CE = dyn_cast<CastExpr>(E))
    return isIntegerAtLeast64(CE->getType(), C.getASTContext());

  return isIntegerAtLeast64(E->getType(), C.getASTContext());
}

bool SAGenTestChecker::isShiftResultUsedAs64(const BinaryOperator *BOp, CheckerContext &C) const {
  ASTContext &ACtx = C.getASTContext();
  const Stmt *Cur = BOp;

  while (Cur) {
    DynTypedNodeList Parents = ACtx.getParents(*Cur);
    if (Parents.empty())
      return false;

    const DynTypedNode &Parent = Parents[0];

    if (const auto *VD = Parent.get<VarDecl>())
      return isIntegerAtLeast64(VD->getType(), ACtx);

    const Stmt *ParentStmt = Parent.get<Stmt>();
    if (!ParentStmt)
      return false;

    if (const auto *CE = dyn_cast<CastExpr>(ParentStmt)) {
      if (isIntegerAtLeast64(CE->getType(), ACtx))
        return true;
      Cur = CE;
      continue;
    }

    if (const auto *PE = dyn_cast<ParenExpr>(ParentStmt)) {
      Cur = PE;
      continue;
    }

    if (const auto *BO = dyn_cast<BinaryOperator>(ParentStmt)) {
      if (BO->isAssignmentOp() && BO->getRHS()->IgnoreParenImpCasts() == BOp)
        return isIntegerAtLeast64(BO->getLHS()->getType(), ACtx);
      return false;
    }

    if (const auto *CE = dyn_cast<CallExpr>(ParentStmt)) {
      const FunctionDecl *FD = CE->getDirectCallee();
      if (!FD)
        return false;

      for (unsigned I = 0; I < CE->getNumArgs() && I < FD->getNumParams(); ++I) {
        if (CE->getArg(I)->IgnoreParenImpCasts() == BOp)
          return isIntegerAtLeast64(FD->getParamDecl(I)->getType(), ACtx);
      }
      return false;
    }

    return false;
  }

  return false;
}

void SAGenTestChecker::checkPreStmt(const BinaryOperator *BOp, CheckerContext &C) const {
  if (BOp->getOpcode() != BO_Shl)
    return;

  const Expr *LHS = BOp->getLHS();
  if (!LHS)
    return;

  if (isWidenedBeforeShift(LHS, C))
    return;

  QualType LHSType = LHS->getType();
  if (!LHSType->isIntegerType())
    return;

  if (C.getASTContext().getTypeSize(LHSType) >= 64)
    return;

  if (!isShiftResultUsedAs64(BOp, C))
    return;

  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto report = std::make_unique<PathSensitiveBugReport>(
      *BT, "Potential integer overflow: left shift is evaluated in a narrow type before being widened to 64 bits", N);
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
