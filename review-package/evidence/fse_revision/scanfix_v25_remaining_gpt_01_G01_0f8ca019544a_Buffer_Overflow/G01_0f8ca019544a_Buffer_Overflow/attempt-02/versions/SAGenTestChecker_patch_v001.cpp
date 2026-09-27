#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Checkers/Taint.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/StmtVisitor.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/StaticAnalyzer/Checkers/utility.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/Decl.h"

using namespace clang;
using namespace ento;
using namespace taint;

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

  // Helper: is the given expression exactly a reference to LoopVD?
  static bool isRefToVar(const Expr *E, const VarDecl *VD);

  // A break guard is safe only when it limits this induction variable to the
  // indexed object's capacity before the access.
  bool hasCapacityBreakGuard(const ForStmt *FS, const ArraySubscriptExpr *ASE,
                             const VarDecl *LoopVD, uint64_t ArraySize,
                             CheckerContext &C) const;
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
  if (!EvaluateExprToInt(N, BO->getRHS(), C)) {
    BoundExcl = llvm::APSInt(64, true);
    BoundExcl = UINT64_MAX;
    return true;
  }

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

bool SAGenTestChecker::hasCapacityBreakGuard(const ForStmt *FS,
                                              const ArraySubscriptExpr *ASE,
                                              const VarDecl *LoopVD,
                                              uint64_t ArraySize,
                                              CheckerContext &C) const {
  const auto *Body = dyn_cast_or_null<CompoundStmt>(FS ? FS->getBody() : nullptr);
  if (!Body || !ASE || !LoopVD)
    return false;

  for (const Stmt *S : Body->body()) {
    const auto *If = dyn_cast<IfStmt>(S);
    if (!If || !If->getBeginLoc().isBeforeInTranslationUnit(ASE->getBeginLoc()))
      continue;

    const Stmt *Then = If->getThen();
    if (!isa<BreakStmt>(Then)) {
      const auto *ThenBody = dyn_cast_or_null<CompoundStmt>(Then);
      if (!ThenBody || ThenBody->size() != 1 ||
          !isa<BreakStmt>(*ThenBody->body_begin()))
        continue;
    }

    const auto *Cond = dyn_cast<BinaryOperator>(
        If->getCond()->IgnoreParenImpCasts());
    if (!Cond || Cond->getOpcode() != BO_GE ||
        !isRefToVar(Cond->getLHS(), LoopVD))
      continue;

    llvm::APSInt Limit;
    if (EvaluateExprToInt(Limit, Cond->getRHS(), C) &&
        Limit.getLimitedValue(UINT64_MAX) <= ArraySize)
      return true;
  }
  return false;
}

void SAGenTestChecker::checkPostStmt(const ArraySubscriptExpr *ASE,
                                     CheckerContext &C) const {
  if (!ASE)
    return;

  // The indexed induction variable may belong to an outer loop when the
  // access is inside a nested loop.
  const Expr *Idx = ASE->getIdx()->IgnoreParenImpCasts();
  const auto *IdxDRE = dyn_cast<DeclRefExpr>(Idx);
  const auto *IndexVD = IdxDRE ? dyn_cast<VarDecl>(IdxDRE->getDecl()) : nullptr;
  if (!IndexVD)
    return;

  const ForStmt *FS = nullptr;
  const ForStmt *GuardLoops[8];
  unsigned GuardLoopCount = 0;
  DynTypedNode Node = DynTypedNode::create(*ASE);
  while (true) {
    const auto Parents = C.getASTContext().getParents(Node);
    if (Parents.empty())
      break;
    Node = Parents[0];
    if (const auto *ParentLoop = Node.get<ForStmt>()) {
      if (GuardLoopCount < sizeof(GuardLoops) / sizeof(GuardLoops[0]))
        GuardLoops[GuardLoopCount++] = ParentLoop;
      const VarDecl *Candidate = getLoopVarFromInit(ParentLoop->getInit(), C);
      if (Candidate == IndexVD) {
        FS = ParentLoop;
        break;
      }
    }
  }
  if (!FS)
    return;

  const VarDecl *LoopVD = IndexVD;
  if (!isIncrementByOne(FS->getInc(), LoopVD, C))
    return;

  llvm::APSInt BoundExcl;
  if (!getExclusiveBoundFromCond(FS->getCond(), LoopVD, BoundExcl, C))
    return;

  // 6) Retrieve the compile-time array size.
  uint64_t ArraySize = 0;
  if (!getConstArraySizeFromBase(ASE->getBase(), ArraySize, C))
    return;

  // A break in a nested loop can guard this access without belonging to the
  // loop that advances the indexed induction variable.
  for (unsigned I = 0; I < GuardLoopCount; ++I) {
    if (hasCapacityBreakGuard(GuardLoops[I], ASE, LoopVD, ArraySize, C))
      return;
  }

  // 7) Compare loop bound and array size.
  uint64_t BoundExclVal = BoundExcl.getLimitedValue(UINT64_MAX);
  if (BoundExclVal > ArraySize) {
    ExplodedNode *N = C.generateNonFatalErrorNode();
    if (!N)
      return;

    auto R = std::make_unique<PathSensitiveBugReport>(
        *BT, "Loop bound exceeds target array size; possible out-of-bounds "
             "index",
        N);
    R->addRange(ASE->getSourceRange());
    C.emitReport(std::move(R));
  }
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
