#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Checkers/Taint.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/Environment.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtVisitor.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/StaticAnalyzer/Checkers/utility.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "llvm/ADT/SmallVector.h"

using namespace clang;
using namespace ento;
using namespace taint;

namespace {

class SAGenTestChecker : public Checker<check::BranchCondition> {
   mutable std::unique_ptr<BugType> BT;

   public:
      SAGenTestChecker()
        : BT(new BugType(this, "Speculative shared read before guard", "Concurrency")) {}

      void checkBranchCondition(const Stmt *Condition, CheckerContext &C) const;

   private:

      // Helpers
      const IfStmt *getEnclosingIf(const Stmt *Condition, CheckerContext &C) const;

      void getParentCompoundAndPrevStmt(const IfStmt *IS,
                                        const CheckerContext &C,
                                        const CompoundStmt *&CS,
                                        const Stmt *&Prev) const;

      const VarDecl *getAssignedVar(const Stmt *S, const Expr *&InitOrRHS) const;

      bool containsDeclRefToVar(const Stmt *S, const VarDecl *VD) const;

      bool containsBitwiseUseOfVar(const Stmt *S, const VarDecl *VD) const {
        if (!S || !VD) return false;
        if (const auto *BO = dyn_cast<BinaryOperator>(S)) {
          if (BO->isBitwiseOp() &&
              (containsDeclRefToVar(BO->getLHS(), VD) ||
               containsDeclRefToVar(BO->getRHS(), VD)))
            return true;
        }
        for (const Stmt *Child : S->children()) {
          if (Child && containsBitwiseUseOfVar(Child, VD))
            return true;
        }
        return false;
      }

      bool exprContainsUseOfVar(const Expr *E, const VarDecl *VD) const {
        if (!E || !VD) return false;
        return containsDeclRefToVar(E, VD);
      }

      bool stmtContainsUseOfVar(const Stmt *S, const VarDecl *VD) const {
        if (!S || !VD) return false;
        return containsDeclRefToVar(S, VD);
      }

      void collectConjuncts(const Expr *Cond, llvm::SmallVector<const Expr*, 8> &Conj) const;

      bool isPotentialRacyRead(const Expr *E, CheckerContext &C) const;

      void report(const Stmt *Highlight, CheckerContext &C) const;
};

const IfStmt *SAGenTestChecker::getEnclosingIf(const Stmt *Condition, CheckerContext &C) const {
  return findSpecificTypeInParents<IfStmt>(Condition, C);
}

void SAGenTestChecker::getParentCompoundAndPrevStmt(const IfStmt *IS,
                                                    const CheckerContext &C,
                                                    const CompoundStmt *&CS,
                                                    const Stmt *&Prev) const {
  CS = findSpecificTypeInParents<CompoundStmt>(IS, const_cast<CheckerContext&>(C));
  Prev = nullptr;
  if (!CS || !IS) return;

  const Stmt *PrevCandidate = nullptr;
  for (const Stmt *S : CS->body()) {
    if (S == IS) {
      Prev = PrevCandidate;
      return;
    }
    PrevCandidate = S;
  }
}

const VarDecl *SAGenTestChecker::getAssignedVar(const Stmt *S, const Expr *&InitOrRHS) const {
  InitOrRHS = nullptr;
  if (!S) return nullptr;

  // Case 1: Declaration with initializer: e.g., "unsigned long x = *p;"
  if (const auto *DS = dyn_cast<DeclStmt>(S)) {
    if (DS->isSingleDecl()) {
      if (const auto *VD = dyn_cast<VarDecl>(DS->getSingleDecl())) {
        if (VD->hasInit()) {
          InitOrRHS = VD->getInit();
          return VD;
        }
      }
    }
  }

  // Case 2: Simple assignment: e.g., "x = *p;"
  if (const auto *ES = dyn_cast<Expr>(S)) {
    const Expr *E = ES->IgnoreParenImpCasts();
    if (const auto *BO = dyn_cast<BinaryOperator>(E)) {
      if (BO->getOpcode() == BO_Assign) {
        const Expr *LHS = BO->getLHS()->IgnoreParenImpCasts();
        if (const auto *DRE = dyn_cast<DeclRefExpr>(LHS)) {
          if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
            InitOrRHS = BO->getRHS();
            return VD;
          }
        }
      }
    }
  }

  return nullptr;
}

bool SAGenTestChecker::containsDeclRefToVar(const Stmt *S, const VarDecl *VD) const {
  if (!S || !VD) return false;

  if (const auto *DRE = dyn_cast<DeclRefExpr>(S)) {
    if (DRE->getDecl() == VD)
      return true;
  }

  for (const Stmt *Child : S->children()) {
    if (Child && containsDeclRefToVar(Child, VD))
      return true;
  }
  return false;
}

void SAGenTestChecker::collectConjuncts(const Expr *Cond, llvm::SmallVector<const Expr*, 8> &Conj) const {
  if (!Cond) return;
  const Expr *E = Cond->IgnoreParenImpCasts();
  if (const auto *BO = dyn_cast<BinaryOperator>(E)) {
    if (BO->getOpcode() == BO_LAnd) {
      collectConjuncts(BO->getLHS(), Conj);
      collectConjuncts(BO->getRHS(), Conj);
      return;
    }
  }
  Conj.push_back(E);
}

bool SAGenTestChecker::isPotentialRacyRead(const Expr *E, CheckerContext &C) const {
  if (!E) return false;

  // Heuristic recursive scan
  std::function<bool(const Stmt*)> Scan = [&](const Stmt *S) -> bool {
    if (!S) return false;

    // Known racy patterns
    if (const auto *UO = dyn_cast<UnaryOperator>(S)) {
      if (UO->getOpcode() == UO_Deref &&
          isa<CallExpr>(UO->getSubExpr()->IgnoreParenImpCasts()))
        return true;
    }
    for (const Stmt *Child : S->children()) {
      if (Child && Scan(Child))
        return true;
    }
    return false;
  };

  return Scan(E);
}

void SAGenTestChecker::report(const Stmt *Highlight, CheckerContext &C) const {
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto R = std::make_unique<PathSensitiveBugReport>(
      *BT, "Speculative read of shared state before guard; move the read inside the guarded branch", N);
  if (Highlight)
    R->addRange(Highlight->getSourceRange());
  C.emitReport(std::move(R));
}

void SAGenTestChecker::checkBranchCondition(const Stmt *Condition, CheckerContext &C) const {
  if (!Condition)
    return;

  const IfStmt *IS = getEnclosingIf(Condition, C);
  if (!IS)
    return;

  // Find the previous statement in the same compound block.
  const CompoundStmt *CS = nullptr;
  const Stmt *Prev = nullptr;
  getParentCompoundAndPrevStmt(IS, C, CS, Prev);
  if (!Prev)
    return;

  // Extract the variable assigned/initialized in Prev and the RHS expression.
  const Expr *InitOrRHS = nullptr;
  const VarDecl *VD = getAssignedVar(Prev, InitOrRHS);
  if (!VD || !InitOrRHS)
    return;

  // A read inside an enclosing branch is already control-dependent on that guard.
  if (getEnclosingIf(Prev, C))
    return;

  // We only care if the previous assignment looks like a potentially racy read.
  if (!isPotentialRacyRead(InitOrRHS, C))
    return;

  // Get the condition expression
  const Expr *CondE = dyn_cast<Expr>(Condition);
  if (!CondE)
    return;
  CondE = CondE->IgnoreParenImpCasts();

  // Report only when a later conjunct consumes the value read before the guard.
  llvm::SmallVector<const Expr*, 8> Conj;
  collectConjuncts(CondE, Conj);
  if (Conj.size() >= 2) {
    const Expr *Guard = Conj[0]->IgnoreParenImpCasts();
    const auto *GuardRef = dyn_cast<DeclRefExpr>(Guard);
    const auto *GuardVar = GuardRef ? dyn_cast<VarDecl>(GuardRef->getDecl()) : nullptr;
    bool IsIndependentModeGuard =
        GuardVar && GuardVar->getType()->isBooleanType() &&
        !exprContainsUseOfVar(Guard, VD);
    unsigned BitwiseTestsOfVD = 0;
    for (size_t i = 1; i < Conj.size(); ++i) {
      if (containsBitwiseUseOfVar(Conj[i], VD))
        ++BitwiseTestsOfVD;
    }
    if (IsIndependentModeGuard && BitwiseTestsOfVD >= 2)
      report(InitOrRHS, C);
  }

  // No state change required; purely structural checker.
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects speculative shared reads performed before a guarding condition",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
