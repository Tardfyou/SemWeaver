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
      // Determine if a CallExpr calls a known allocator (e.g., kzalloc, kmalloc, etc.).
      bool isKnownAllocatorCall(const CallExpr *CE, CheckerContext &C) const;
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

bool SAGenTestChecker::isKnownAllocatorCall(const CallExpr *CE, CheckerContext &C) const {
  if (!CE)
    return false;

  static const char *Allocators[] = {
    "kzalloc", "kmalloc", "kcalloc", "kvzalloc", "vzalloc", "kvmalloc",
    "devm_kzalloc", "devm_kmalloc", "devm_kcalloc"
  };
  if (const FunctionDecl *FD = CE->getDirectCallee()) {
    for (const char *Name : Allocators) {
      if (FD->getName() == Name)
        return true;
    }
  }

  const Expr *CalleeE = CE->getCallee();
  if (!CalleeE)
    return false;
  for (const char *Name : Allocators) {
    if (ExprHasName(CalleeE, Name, C))
      return true;
  }
  return false;
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

  // Search earlier statements for an allocation into a different pointer
  // field of the same object; unrelated intervening statements do not break
  // this value/object relation.
  const CompoundStmt *CS = findSpecificTypeInParents<CompoundStmt>(IfS, C);
  if (!CS || !CheckedPtrExpr->getType()->isPointerType())
    return;

  // Compare the root object in the member-access expressions. Analyzer region
  // lookup can be unavailable for AST expressions that are not bound in the
  // current path state.
  auto getRootObject = [](const Expr *E) -> const ValueDecl * {
    E = E->IgnoreParenImpCasts();
    while (const auto *ME = dyn_cast<MemberExpr>(E))
      E = ME->getBase()->IgnoreParenImpCasts();
    if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
      return DRE->getDecl();
    return nullptr;
  };
  auto getPointerSlot = [](const Expr *E) -> const ValueDecl * {
    E = E->IgnoreParenImpCasts();
    while (const auto *ME = dyn_cast<MemberExpr>(E)) {
      if (isa<MemberExpr>(ME->getBase()->IgnoreParenImpCasts()) ||
          isa<DeclRefExpr>(ME->getBase()->IgnoreParenImpCasts()))
        return ME->getMemberDecl();
      E = ME->getBase()->IgnoreParenImpCasts();
    }
    if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
      return DRE->getDecl();
    return nullptr;
  };
  const ValueDecl *CheckedRoot = getRootObject(CheckedPtrExpr);
  const ValueDecl *CheckedSlot = getPointerSlot(CheckedPtrExpr);
  if (!CheckedRoot || !CheckedSlot)
    return;

  for (auto I = CS->body_begin(), E = CS->body_end(); I != E && *I != IfS; ++I) {
    bool Reported = false;
    std::function<void(const Stmt *)> InspectPrior = [&](const Stmt *S) {
      if (!S || Reported)
        return;

      const Expr *Core = unwrapToCoreExpr(S);
      if (const auto *BO = dyn_cast_or_null<BinaryOperator>(Core)) {
        if (BO->getOpcode() == BO_Assign) {
          const Expr *LHS = BO->getLHS();
          const Expr *RHS = BO->getRHS();
          if (LHS && RHS && LHS->getType()->isPointerType()) {
            const CallExpr *AllocCE = dyn_cast<CallExpr>(RHS->IgnoreParenCasts());
            const ValueDecl *AllocRoot = getRootObject(LHS);
            const ValueDecl *AllocatedSlot = getPointerSlot(LHS);
            if (isKnownAllocatorCall(AllocCE, C) && AllocRoot == CheckedRoot &&
                AllocatedSlot && AllocatedSlot != CheckedSlot) {
              ExplodedNode *N = C.generateNonFatalErrorNode();
              if (!N)
                return;

              auto R = std::make_unique<PathSensitiveBugReport>(
                  *BT, "Wrong pointer checked after allocation", N);
              R->addRange(IfS->getCond()->getSourceRange());
              R->addRange(BO->getSourceRange());
              C.emitReport(std::move(R));
              Reported = true;
              return;
            }
          }
        }
      }

      for (const Stmt *Child : S->children())
        InspectPrior(Child);
    };
    InspectPrior(*I);
    if (Reported)
      return;
  }
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
