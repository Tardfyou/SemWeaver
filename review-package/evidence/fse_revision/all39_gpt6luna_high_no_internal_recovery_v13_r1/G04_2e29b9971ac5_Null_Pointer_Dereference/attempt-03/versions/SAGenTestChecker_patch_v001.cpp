#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Checkers/Taint.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/Environment.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SymExpr.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtVisitor.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/StaticAnalyzer/Checkers/utility.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "clang/Lex/Lexer.h"

using namespace clang;
using namespace ento;
using namespace taint;

namespace {
class SAGenTestChecker : public Checker<check::BranchCondition> {
   mutable std::unique_ptr<BugType> BT;

   public:
      SAGenTestChecker() : BT(new BugType(this, "Wrong NULL check after allocation", "Logic error")) {}

      void checkBranchCondition(const Stmt *Condition, CheckerContext &C) const;

   private:
      // Extract the pointer expression being NULL-checked in a condition.
      const Expr *extractNullCheckedPtr(const Expr *Cond, CheckerContext &C) const;
      // Determine whether a call produces a pointer value for the assignment.
      bool isPointerProducingCall(const CallExpr *CE) const;
      // Identify the root declaration and final field of a member access.
      const ValueDecl *getAccessRoot(const Expr *E) const;
      const ValueDecl *getAccessField(const Expr *E) const;
      // Unwrap wrappers to reach the core expression (e.g., ExprWithCleanups).
      const Expr *unwrapToCoreExpr(const Stmt *S) const;
};

const Expr *SAGenTestChecker::unwrapToCoreExpr(const Stmt *S) const {
  const Stmt *Cur = S;
  while (true) {
    if (const auto *EWC = dyn_cast<ExprWithCleanups>(Cur)) {
      Cur = EWC->getSubExpr();
      continue;
    }
    if (const auto *FE = dyn_cast<FullExpr>(Cur)) {
      Cur = FE->getSubExpr();
      continue;
    }
    break;
  }
  return dyn_cast<Expr>(Cur);
}

bool SAGenTestChecker::isPointerProducingCall(const CallExpr *CE) const {
  return CE && CE->getType()->isPointerType();
}

const ValueDecl *SAGenTestChecker::getAccessRoot(const Expr *E) const {
  if (!E)
    return nullptr;
  E = E->IgnoreParenImpCasts();
  while (const auto *ME = dyn_cast<MemberExpr>(E))
    E = ME->getBase()->IgnoreParenImpCasts();
  if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
    return DRE->getDecl();
  return nullptr;
}

const ValueDecl *SAGenTestChecker::getAccessField(const Expr *E) const {
  if (!E)
    return nullptr;
  E = E->IgnoreParenImpCasts();
  if (const auto *ME = dyn_cast<MemberExpr>(E))
    return ME->getMemberDecl();
  return nullptr;
}

const Expr *SAGenTestChecker::extractNullCheckedPtr(const Expr *Cond, CheckerContext &C) const {
  if (!Cond)
    return nullptr;

  ASTContext &ACtx = C.getASTContext();
  const Expr *E = Cond->IgnoreParenCasts();

  if (const auto *UO = dyn_cast<UnaryOperator>(E)) {
    if (UO->getOpcode() == UO_LNot) {
      const Expr *SubE = UO->getSubExpr()->IgnoreParenCasts();
      if (SubE && SubE->getType()->isPointerType())
        return SubE;
    }
  } else if (const auto *BO = dyn_cast<BinaryOperator>(E)) {
    BinaryOperator::Opcode Op = BO->getOpcode();
    if (Op == BO_EQ) {
      const Expr *LHS = BO->getLHS()->IgnoreParenCasts();
      const Expr *RHS = BO->getRHS()->IgnoreParenCasts();

      bool LHSIsNull = LHS->isNullPointerConstant(ACtx, Expr::NPC_ValueDependentIsNull);
      bool RHSIsNull = RHS->isNullPointerConstant(ACtx, Expr::NPC_ValueDependentIsNull);

      if (LHSIsNull && RHS && RHS->getType()->isPointerType())
        return RHS;
      if (RHSIsNull && LHS && LHS->getType()->isPointerType())
        return LHS;
    }
  }
  return nullptr;
}

void SAGenTestChecker::checkBranchCondition(const Stmt *Condition, CheckerContext &C) const {
  if (!Condition)
    return;

  // Find the enclosing IfStmt for this condition.
  const IfStmt *IfS = findSpecificTypeInParents<IfStmt>(Condition, C);
  if (!IfS)
    return;

  const Expr *CondE = dyn_cast<Expr>(Condition);
  if (!CondE)
    return;

  // Extract the pointer being NULL-checked in the condition.
  const Expr *CheckedPtrExpr = extractNullCheckedPtr(CondE, C);
  if (!CheckedPtrExpr)
    return;

  // Find the nearest earlier allocator assignment that targets a different
  // pointer field of the same object. Non-assignment statements between the
  // allocation and check do not change that object/value relation.
  const CompoundStmt *CS = findSpecificTypeInParents<CompoundStmt>(IfS, C);
  if (!CS || !CheckedPtrExpr->getType()->isPointerType())
    return;

  const ValueDecl *CheckedRoot = getAccessRoot(CheckedPtrExpr);
  const ValueDecl *CheckedField = getAccessField(CheckedPtrExpr);
  if (!CheckedRoot || !CheckedField)
    return;

  const BinaryOperator *BO = nullptr;
  for (auto I = CS->body_begin(), E = CS->body_end(); I != E; ++I) {
    if (*I != IfS)
      continue;

    while (I != CS->body_begin()) {
      --I;
      const Expr *Candidate = unwrapToCoreExpr(*I);
      const auto *Assignment = dyn_cast_or_null<BinaryOperator>(Candidate);
      if (!Assignment || Assignment->getOpcode() != BO_Assign)
        continue;

      const Expr *LHS = Assignment->getLHS();
      const Expr *RHS = Assignment->getRHS();
      if (!LHS || !RHS || !LHS->getType()->isPointerType())
        continue;

      const ValueDecl *LhsRoot = getAccessRoot(LHS);
      const ValueDecl *LhsField = getAccessField(LHS);
      if (LhsRoot == CheckedRoot && LhsField == CheckedField)
        break;

      const CallExpr *AllocCE = dyn_cast<CallExpr>(RHS->IgnoreParenCasts());
      if (!isPointerProducingCall(AllocCE))
        continue;

      if (LhsRoot != CheckedRoot || !LhsField || LhsField == CheckedField)
        continue;

      BO = Assignment;
      break;
    }
    break;
  }
  if (!BO)
    return;

  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto R = std::make_unique<PathSensitiveBugReport>(
      *BT, "Wrong pointer checked after allocation", N);
  R->addRange(IfS->getCond()->getSourceRange());
  R->addRange(BO->getSourceRange());
  C.emitReport(std::move(R));
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects checking a different pointer than the one just allocated (wrong NULL check)",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
