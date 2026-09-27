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
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtVisitor.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/StaticAnalyzer/Checkers/utility.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/Decl.h"

using namespace clang;
using namespace ento;
using namespace taint;

// No custom program state is required.

namespace {

class SAGenTestChecker : public Checker<check::PostStmt<ArraySubscriptExpr>> {
   mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Loop bound exceeds array size",
                       "Array bounds")) {}

  void checkPostStmt(const ArraySubscriptExpr *ASE, CheckerContext &C) const;

private:
  // Helper: get loop variable from ForStmt init and ensure it's initialized to 0.
  const VarDecl *getLoopVarFromInit(const Stmt *Init, CheckerContext &C) const;

  // Helper: check increment is ++i, i++, or i += 1 on the loop variable.
  bool isIncrementByOne(const Stmt *IncS, const VarDecl *LoopVD,
                        CheckerContext &C) const;

  // Helper: extract exclusive upper bound from condition "i < N" or "i <= N".
  bool getExclusiveBoundFromCond(const Expr *CondE, const VarDecl *LoopVD,
                                 llvm::APSInt &BoundExcl,
                                 CheckerContext &C) const;

  // Helper: get compile-time array size from base expression.
  bool getConstArraySizeFromBase(const Expr *BaseE, uint64_t &ArraySize,
                                 CheckerContext &C) const;

  // Helper: is the condition structurally "LoopVD < N" or "LoopVD <= N"?
  static bool isIndexBoundCondition(const Expr *CondE, const VarDecl *LoopVD);

  // Helper: find the enclosing for-statement that drives LoopVD (it starts
  // the variable at zero and steps it by one); enclosing loops over other
  // counters are transparent, so the driving loop may be an outer loop.
  const ForStmt *findBindingLoopForVar(const Stmt *Start,
                                       const VarDecl *LoopVD,
                                       CheckerContext &C) const;

  // Helper: does the statement subtree contain a guard that breaks out once
  // LoopVD reaches the compile-time capacity Cap?
  bool hasCapacityBreakGuard(const Stmt *Body, const VarDecl *LoopVD,
                             uint64_t Cap, CheckerContext &C) const;

  // Helper: is the condition "LoopVD >= Cap", "Cap <= LoopVD",
  // "LoopVD > Cap - 1", "Cap - 1 < LoopVD", or "LoopVD == Cap"?
  bool isCapacityGuardCondition(const Expr *CondE, const VarDecl *LoopVD,
                                uint64_t Cap, CheckerContext &C) const;

  // Helper: does the statement subtree contain a break statement?
  static bool containsBreak(const Stmt *S);

  // Helper: is the given expression exactly a reference to LoopVD?
  static bool isRefToVar(const Expr *E, const VarDecl *VD);
};

bool SAGenTestChecker::isRefToVar(const Expr *E, const VarDecl *VD) {
  if (!E || !VD)
    return false;
  E = E->IgnoreParenImpCasts();
  if (const auto *DRE = dyn_cast<DeclRefExpr>(E)) {
    return DRE->getDecl() == VD;
  }
  return false;
}

const VarDecl *
SAGenTestChecker::getLoopVarFromInit(const Stmt *Init, CheckerContext &C) const {
  if (!Init)
    return nullptr;

  // Case 1: int i = 0;
  if (const auto *DS = dyn_cast<DeclStmt>(Init)) {
    if (DS->isSingleDecl()) {
      if (const auto *VD = dyn_cast<VarDecl>(DS->getSingleDecl())) {
        if (VD->hasInit()) {
          llvm::APSInt Val;
          if (EvaluateExprToInt(Val, VD->getInit(), C)) {
            if (Val == 0)
              return VD;
          }
        }
      }
    }
    return nullptr;
  }

  // Case 2: i = 0;
  if (const auto *BO = dyn_cast<BinaryOperator>(Init)) {
    if (BO->getOpcode() == BO_Assign) {
      const Expr *LHS = BO->getLHS()->IgnoreParenCasts();
      const auto *DRE = dyn_cast<DeclRefExpr>(LHS);
      const auto *VD = DRE ? dyn_cast<VarDecl>(DRE->getDecl()) : nullptr;
      if (!VD)
        return nullptr;

      llvm::APSInt Val;
      if (EvaluateExprToInt(Val, BO->getRHS(), C) && Val == 0)
        return VD;
    }
  }

  return nullptr;
}

bool SAGenTestChecker::isIncrementByOne(const Stmt *IncS, const VarDecl *LoopVD,
                                        CheckerContext &C) const {
  if (!IncS || !LoopVD)
    return false;

  const auto *IncE = dyn_cast<Expr>(IncS);
  if (!IncE)
    return false;

  const Expr *E = IncE->IgnoreParenCasts();

  // ++i or i++
  if (const auto *UO = dyn_cast<UnaryOperator>(E)) {
    if ((UO->getOpcode() == UO_PreInc || UO->getOpcode() == UO_PostInc) &&
        isRefToVar(UO->getSubExpr(), LoopVD))
      return true;
    return false;
  }

  // i += 1
  if (const auto *BO = dyn_cast<BinaryOperator>(E)) {
    if (BO->getOpcode() == BO_AddAssign && isRefToVar(BO->getLHS(), LoopVD)) {
      llvm::APSInt Val;
      if (EvaluateExprToInt(Val, BO->getRHS(), C) && Val == 1)
        return true;
    }
    return false;
  }

  return false;
}

bool SAGenTestChecker::getExclusiveBoundFromCond(const Expr *CondE,
                                                 const VarDecl *LoopVD,
                                                 llvm::APSInt &BoundExcl,
                                                 CheckerContext &C) const {
  if (!CondE || !LoopVD)
    return false;

  const auto *BO = dyn_cast<BinaryOperator>(CondE->IgnoreParenCasts());
  if (!BO)
    return false;

  BinaryOperator::Opcode Op = BO->getOpcode();
  if (Op != BO_LT && Op != BO_LE)
    return false;

  // LHS must be the loop var
  if (!isRefToVar(BO->getLHS(), LoopVD))
    return false;

  llvm::APSInt N;
  if (!EvaluateExprToInt(N, BO->getRHS(), C))
    return false;

  BoundExcl = N;
  if (Op == BO_LE) {
    llvm::APSInt One(N.getBitWidth(), N.isUnsigned());
    One = 1;
    BoundExcl = N + One;
  }
  return true;
}

bool SAGenTestChecker::getConstArraySizeFromBase(const Expr *BaseE,
                                                 uint64_t &ArraySize,
                                                 CheckerContext &C) const {
  if (!BaseE)
    return false;

  BaseE = BaseE->IgnoreParenImpCasts();

  // MemberExpr: struct_field_array[i]
  if (const auto *ME = dyn_cast<MemberExpr>(BaseE)) {
    const ValueDecl *VD = ME->getMemberDecl();
    const auto *FD = dyn_cast<FieldDecl>(VD);
    if (!FD)
      return false;

    QualType QT = FD->getType();
    const ConstantArrayType *CAT =
        C.getASTContext().getAsConstantArrayType(QT);
    if (!CAT)
      return false;

    ArraySize = CAT->getSize().getZExtValue();
    return true;
  }

  // DeclRefExpr: local/global array
  if (const auto *DRE = dyn_cast<DeclRefExpr>(BaseE)) {
    llvm::APInt Sz;
    if (getArraySizeFromExpr(Sz, DRE)) {
      ArraySize = Sz.getZExtValue();
      return true;
    }
  }

  return false;
}

bool SAGenTestChecker::isIndexBoundCondition(const Expr *CondE,
                                             const VarDecl *LoopVD) {
  if (!CondE || !LoopVD)
    return false;

  const auto *BO = dyn_cast<BinaryOperator>(CondE->IgnoreParenCasts());
  if (!BO)
    return false;

  BinaryOperator::Opcode Op = BO->getOpcode();
  if (Op != BO_LT && Op != BO_LE)
    return false;

  return isRefToVar(BO->getLHS(), LoopVD);
}

const ForStmt *SAGenTestChecker::findBindingLoopForVar(
    const Stmt *Start, const VarDecl *LoopVD, CheckerContext &C) const {
  if (!Start || !LoopVD)
    return nullptr;

  ASTContext &ACtx = C.getASTContext();
  const Stmt *Child = Start;
  while (Child) {
    auto Parents = ACtx.getParents<Stmt>(*Child);
    if (Parents.size() == 0)
      break;

    const Stmt *Parent = Parents[0].get<Stmt>();
    if (!Parent)
      break;

    if (const auto *FS = dyn_cast<ForStmt>(Parent)) {
      // A loop drives the index when it starts it at zero, steps it by one,
      // and uses it in the bound condition. Enclosing loops over other
      // counters are transparent, so an index consumed inside a nested loop
      // resolves to its driving outer loop.
      if (getLoopVarFromInit(FS->getInit(), C) == LoopVD &&
          isIncrementByOne(FS->getInc(), LoopVD, C) &&
          isIndexBoundCondition(FS->getCond(), LoopVD))
        return FS;
    }

    Child = Parent;
  }
  return nullptr;
}

bool SAGenTestChecker::containsBreak(const Stmt *S) {
  if (!S)
    return false;
  if (isa<BreakStmt>(S))
    return true;
  for (const Stmt *Child : S->children()) {
    if (Child && containsBreak(Child))
      return true;
  }
  return false;
}

bool SAGenTestChecker::isCapacityGuardCondition(const Expr *CondE,
                                                const VarDecl *LoopVD,
                                                uint64_t Cap,
                                                CheckerContext &C) const {
  if (!CondE || !LoopVD)
    return false;

  const auto *BO = dyn_cast<BinaryOperator>(CondE->IgnoreParenImpCasts());
  if (!BO)
    return false;

  BinaryOperator::Opcode Op = BO->getOpcode();
  if (Op != BO_GE && Op != BO_GT && Op != BO_LE && Op != BO_LT && Op != BO_EQ)
    return false;

  const Expr *LHS = BO->getLHS()->IgnoreParenImpCasts();
  const Expr *RHS = BO->getRHS()->IgnoreParenImpCasts();

  llvm::APSInt V;
  // "LoopVD >= Cap", "LoopVD == Cap", or "LoopVD > Cap - 1"
  if (isRefToVar(LHS, LoopVD) && !isRefToVar(RHS, LoopVD) &&
      EvaluateExprToInt(V, RHS, C)) {
    if (V.isNegative())
      return false;
    uint64_t K = V.getLimitedValue(UINT64_MAX);
    if ((Op == BO_GE || Op == BO_EQ) && K == Cap)
      return true;
    if (Op == BO_GT && Cap > 0 && K == Cap - 1)
      return true;
    return false;
  }

  // "Cap <= LoopVD", "Cap == LoopVD", or "Cap - 1 < LoopVD"
  if (isRefToVar(RHS, LoopVD) && !isRefToVar(LHS, LoopVD) &&
      EvaluateExprToInt(V, LHS, C)) {
    if (V.isNegative())
      return false;
    uint64_t K = V.getLimitedValue(UINT64_MAX);
    if ((Op == BO_LE || Op == BO_EQ) && K == Cap)
      return true;
    if (Op == BO_LT && Cap > 0 && K == Cap - 1)
      return true;
    return false;
  }

  return false;
}

bool SAGenTestChecker::hasCapacityBreakGuard(const Stmt *Body,
                                             const VarDecl *LoopVD,
                                             uint64_t Cap,
                                             CheckerContext &C) const {
  if (!Body)
    return false;

  if (const auto *IS = dyn_cast<IfStmt>(Body)) {
    if (isCapacityGuardCondition(IS->getCond(), LoopVD, Cap, C) &&
        (containsBreak(IS->getThen()) || containsBreak(IS->getElse())))
      return true;
  }

  for (const Stmt *Child : Body->children()) {
    if (Child && hasCapacityBreakGuard(Child, LoopVD, Cap, C))
      return true;
  }
  return false;
}

void SAGenTestChecker::checkPostStmt(const ArraySubscriptExpr *ASE,
                                     CheckerContext &C) const {
  if (!ASE)
    return;

  // 1) The index must be a plain variable.
  const Expr *Idx = ASE->getIdx()->IgnoreParenImpCasts();
  const auto *IdxDRE = dyn_cast<DeclRefExpr>(Idx);
  if (!IdxDRE)
    return;
  const auto *LoopVD = dyn_cast<VarDecl>(IdxDRE->getDecl());
  if (!LoopVD)
    return;

  // 2) Find the for-statement that drives this index variable: it starts it
  //    at zero and steps it by one. When the access sits in a nested loop,
  //    the driving loop can be an outer one.
  const ForStmt *FS = findBindingLoopForVar(ASE, LoopVD, C);
  if (!FS)
    return;

  // 3) Retrieve the compile-time capacity of the indexed array.
  uint64_t ArraySize = 0;
  if (!getConstArraySizeFromBase(ASE->getBase(), ArraySize, C))
    return;

  // 4) Relate the driving loop's bound to the array capacity.
  llvm::APSInt BoundExcl;
  if (getExclusiveBoundFromCond(FS->getCond(), LoopVD, BoundExcl, C)) {
    // Statically provable bound.
    uint64_t BoundExclVal = BoundExcl.getLimitedValue(UINT64_MAX);
    if (BoundExclVal <= ArraySize)
      return; // the loop cannot reach past the end of the array

    // Even an over-large constant bound is harmless when the loop body
    // carries a barrier that stops the index at the capacity.
    if (hasCapacityBreakGuard(FS->getBody(), LoopVD, ArraySize, C))
      return;

    ExplodedNode *N = C.generateNonFatalErrorNode();
    if (!N)
      return;

    auto R = std::make_unique<PathSensitiveBugReport>(
        *BT, "Loop bound exceeds target array size; possible out-of-bounds "
             "index",
        N);
    R->addRange(ASE->getSourceRange());
    C.emitReport(std::move(R));
    return;
  }

  // 5) The loop bound is not a compile-time constant, so it cannot be proven
  //    to keep the index below the array capacity. The safety barrier for
  //    such loops is a guard that breaks out once the index reaches the
  //    capacity; without it the index can run past the end of the array.
  if (hasCapacityBreakGuard(FS->getBody(), LoopVD, ArraySize, C))
    return;

  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto R = std::make_unique<PathSensitiveBugReport>(
      *BT, "Loop bound is not provably below the array capacity and no "
           "in-loop guard stops at the capacity; possible out-of-bounds index",
      N);
  R->addRange(ASE->getSourceRange());
  C.emitReport(std::move(R));
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects array indexing by loop variable when loop bound exceeds the "
      "array's compile-time size",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
