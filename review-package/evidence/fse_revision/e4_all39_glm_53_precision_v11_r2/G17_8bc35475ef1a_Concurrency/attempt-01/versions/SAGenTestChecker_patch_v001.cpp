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
#include "llvm/ADT/SmallVector.h"

using namespace clang;
using namespace ento;
using namespace taint;

// No custom program states needed.

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

      bool exprContainsUseOfVar(const Expr *E, const VarDecl *VD) const {
        if (!E || !VD) return false;
        return containsDeclRefToVar(E, VD);
      }

      bool stmtContainsUseOfVar(const Stmt *S, const VarDecl *VD) const {
        if (!S || !VD) return false;
        return containsDeclRefToVar(S, VD);
      }

      void collectConjuncts(const Expr *Cond, llvm::SmallVector<const Expr*, 8> &Conj) const;

      bool isSharedAccessorLoad(const Expr *E) const;

      const Stmt *findSpeculativeLoadBefore(const IfStmt *IS,
                                            const CompoundStmt *CS,
                                            const VarDecl *&VD,
                                            const Expr *&RHS) const;

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

// A shared-state accessor load: the dereferenced address is produced by a
// call that hands out a pointer into the internals of a shared object, as
// opposed to a member of a locally tracked value or a macro-expanded
// cast-of-address read.
bool SAGenTestChecker::isSharedAccessorLoad(const Expr *E) const {
  if (!E)
    return false;
  const Expr *Ex = E->IgnoreParenImpCasts();
  const auto *UO = dyn_cast<UnaryOperator>(Ex);
  if (!UO || UO->getOpcode() != UO_Deref)
    return false;
  return isa<CallExpr>(UO->getSubExpr()->IgnoreParenImpCasts());
}

// Walk backwards from the guarded statement to the nearest assignment of a
// shared-state load. The read is speculative only if nothing between the
// load and the guard consumes its value: the guard alone decides whether
// the loaded data is needed at all.
const Stmt *SAGenTestChecker::findSpeculativeLoadBefore(const IfStmt *IS,
                                                        const CompoundStmt *CS,
                                                        const VarDecl *&VD,
                                                        const Expr *&RHS) const {
  VD = nullptr;
  RHS = nullptr;
  if (!IS || !CS)
    return nullptr;

  llvm::SmallVector<const Stmt *, 32> Body;
  bool Found = false;
  for (const Stmt *S : CS->body()) {
    if (S == IS) {
      Found = true;
      break;
    }
    Body.push_back(S);
  }
  if (!Found)
    return nullptr;

  for (int I = static_cast<int>(Body.size()) - 1; I >= 0; --I) {
    const Expr *CandidateRHS = nullptr;
    const VarDecl *CandidateVD = getAssignedVar(Body[I], CandidateRHS);
    if (!CandidateVD || !CandidateRHS)
      continue;
    if (!isSharedAccessorLoad(CandidateRHS))
      continue;

    // An intervening use consumes the value regardless of the guard, so the
    // load is not speculative with respect to this guard.
    bool InterveningUse = false;
    for (size_t J = static_cast<size_t>(I) + 1; J < Body.size(); ++J) {
      if (containsDeclRefToVar(Body[J], CandidateVD)) {
        InterveningUse = true;
        break;
      }
    }
    if (InterveningUse)
      continue;

    VD = CandidateVD;
    RHS = CandidateRHS;
    return Body[I];
  }
  return nullptr;
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

  // Find the enclosing compound block of the guarded statement.
  const CompoundStmt *CS = nullptr;
  const Stmt *Prev = nullptr;
  getParentCompoundAndPrevStmt(IS, C, CS, Prev);
  if (!CS)
    return;

  // Locate the nearest shared-state load whose value reaches the guard
  // without any intervening use: the read runs unconditionally, while its
  // value is needed only once the guard is taken.
  const VarDecl *VD = nullptr;
  const Expr *InitOrRHS = nullptr;
  if (!findSpeculativeLoadBefore(IS, CS, VD, InitOrRHS))
    return;

  // Get the condition expression
  const Expr *CondE = dyn_cast<Expr>(Condition);
  if (!CondE)
    return;
  CondE = CondE->IgnoreParenImpCasts();

  // The loaded value must be confined to the guarded region: it is neither
  // consumed on the unguarded (else) path nor after the guarded construct,
  // so only the guard makes the earlier read necessary.
  const Stmt *ElseS = IS->getElse();
  if (ElseS && stmtContainsUseOfVar(ElseS, VD))
    return;

  bool PastGuard = false;
  for (const Stmt *S : CS->body()) {
    if (S == IS) {
      PastGuard = true;
      continue;
    }
    if (PastGuard && containsDeclRefToVar(S, VD))
      return;
  }

  bool Reported = false;

  // Shape 1: if (Guard && Uses(VD) && ...)
  {
    llvm::SmallVector<const Expr*, 8> Conj;
    collectConjuncts(CondE, Conj);
    if (Conj.size() >= 2) {
      const Expr *Guard = Conj[0];
      bool GuardUsesVD = exprContainsUseOfVar(Guard, VD);
      bool RestUseVD = false;
      for (size_t i = 1; i < Conj.size(); ++i) {
        if (exprContainsUseOfVar(Conj[i], VD)) {
          RestUseVD = true;
          break;
        }
      }
      if (!GuardUsesVD && RestUseVD) {
        report(InitOrRHS, C);
        Reported = true;
      }
    }
  }

  // Shape 2: if (Guard) { Uses(VD) }, and Cond doesn't use VD
  if (!Reported) {
    if (!exprContainsUseOfVar(CondE, VD)) {
      const Stmt *ThenS = IS->getThen();
      if (ThenS && stmtContainsUseOfVar(ThenS, VD)) {
        report(InitOrRHS, C);
        Reported = true;
      }
    }
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
