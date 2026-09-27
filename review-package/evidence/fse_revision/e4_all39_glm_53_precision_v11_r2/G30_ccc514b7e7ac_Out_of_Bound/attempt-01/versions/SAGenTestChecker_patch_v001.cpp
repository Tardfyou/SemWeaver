#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtVisitor.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/StaticAnalyzer/Checkers/utility.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Type.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/APSInt.h"
#include <string>
#include <utility>

using namespace clang;
using namespace ento;

namespace {
/* The checker callbacks are to be decided. */
class SAGenTestChecker : public Checker<check::ASTCodeBody> {
   mutable std::unique_ptr<BugType> BT;

   public:
      SAGenTestChecker() : BT(new BugType(this, "Parallel-array index overflow", "Array bounds")) {}

      void checkASTCodeBody(const Decl *D, AnalysisManager &Mgr, BugReporter &BR) const;

   private:

      // Helpers for loop recognition and array access analysis
      static bool getCanonicalLoop(const ForStmt *FS,
                                   const VarDecl *&LoopVar,
                                   const Expr *&BoundExpr,
                                   bool &IsStrictLess,
                                   ASTContext &Ctx);

      static bool evalToInt(const Expr *E, llvm::APSInt &Out, ASTContext &Ctx);

      static bool indexIsLoopVar(const Expr *Idx, const VarDecl *V);

      using AccessRecord = std::pair<const ArraySubscriptExpr *, uint64_t>;

      static bool thenAlwaysExits(const Stmt *S);

      static bool guardLimitsIndex(const IfStmt *IS, const VarDecl *V,
                                   ASTContext &Ctx, uint64_t &MaxIdxAfter);

      static void collectAccessesFlat(const Stmt *S, const VarDecl *V,
                                      uint64_t MaxIdx,
                                      llvm::SmallVector<AccessRecord, 8> &Out);

      static void collectGuardedAccesses(const Stmt *S, const VarDecl *V,
                                         uint64_t MaxIdx, ASTContext &Ctx,
                                         llvm::SmallVector<AccessRecord, 8> &Out);

      static bool getArraySizeFromSubscriptBase(const Expr *Base, llvm::APInt &ArraySize, ASTContext &Ctx);

      static std::string getArrayName(const Expr *Base);

      void report(const ArraySubscriptExpr *ASE,
                  uint64_t BoundVal,
                  StringRef ArrName,
                  uint64_t ArrSize,
                  BugReporter &BR,
                  ASTContext &Ctx) const;
};

//========================== Helper Implementations ==========================//

bool SAGenTestChecker::evalToInt(const Expr *E, llvm::APSInt &Out, ASTContext &Ctx) {
  if (!E)
    return false;
  Expr::EvalResult ER;
  if (E->EvaluateAsInt(ER, Ctx)) {
    Out = ER.Val.getInt();
    return true;
  }
  return false;
}

bool SAGenTestChecker::getCanonicalLoop(const ForStmt *FS,
                                        const VarDecl *&LoopVar,
                                        const Expr *&BoundExpr,
                                        bool &IsStrictLess,
                                        ASTContext &Ctx) {
  LoopVar = nullptr;
  BoundExpr = nullptr;
  IsStrictLess = true;

  if (!FS)
    return false;

  // 1) Init: either "int i = 0;" or "i = 0;"
  const Stmt *InitS = FS->getInit();
  const VarDecl *V = nullptr;

  if (const auto *DS = dyn_cast_or_null<DeclStmt>(InitS)) {
    if (!DS->isSingleDecl())
      return false;
    const auto *VD = dyn_cast<VarDecl>(DS->getSingleDecl());
    if (!VD || !VD->hasInit())
      return false;
    llvm::APSInt InitVal;
    if (!evalToInt(VD->getInit()->IgnoreParenImpCasts(), InitVal, Ctx))
      return false;
    if (InitVal != 0)
      return false;
    V = VD;
  } else if (const auto *BO = dyn_cast_or_null<BinaryOperator>(InitS)) {
    if (BO->getOpcode() != BO_Assign)
      return false;
    const auto *LHS = dyn_cast<DeclRefExpr>(BO->getLHS()->IgnoreParenImpCasts());
    if (!LHS)
      return false;
    const auto *VD = dyn_cast<VarDecl>(LHS->getDecl());
    if (!VD)
      return false;
    llvm::APSInt InitVal;
    if (!evalToInt(BO->getRHS()->IgnoreParenImpCasts(), InitVal, Ctx))
      return false;
    if (InitVal != 0)
      return false;
    V = VD;
  } else {
    return false;
  }

  // 2) Condition: "i < Bound" or "i <= Bound"
  const Expr *CondE = FS->getCond();
  if (!CondE)
    return false;
  CondE = CondE->IgnoreParenImpCasts();
  const auto *CBO = dyn_cast<BinaryOperator>(CondE);
  if (!CBO)
    return false;

  BinaryOperator::Opcode Op = CBO->getOpcode();
  if (Op != BO_LT && Op != BO_LE)
    return false;

  const auto *L = dyn_cast<DeclRefExpr>(CBO->getLHS()->IgnoreParenImpCasts());
  if (!L)
    return false;
  const auto *LVD = dyn_cast<VarDecl>(L->getDecl());
  if (!LVD || LVD != V)
    return false;

  IsStrictLess = (Op == BO_LT);
  BoundExpr = CBO->getRHS();

  // We do not strictly enforce increment pattern, as per plan.

  LoopVar = V;
  return true;
}

bool SAGenTestChecker::indexIsLoopVar(const Expr *Idx, const VarDecl *V) {
  if (!Idx || !V)
    return false;
  Idx = Idx->IgnoreParenImpCasts();
  if (const auto *DRE = dyn_cast<DeclRefExpr>(Idx)) {
    return DRE->getDecl() == V;
  }
  return false;
}

// True when a statement sequence cannot fall through: it contains an
// unconditional break/continue/return at its own level (exits nested inside
// a further conditional do not count).
bool SAGenTestChecker::thenAlwaysExits(const Stmt *S) {
  if (!S)
    return false;
  if (isa<BreakStmt>(S) || isa<ContinueStmt>(S) || isa<ReturnStmt>(S))
    return true;
  if (const auto *CS = dyn_cast<CompoundStmt>(S)) {
    for (const Stmt *Sub : CS->body())
      if (thenAlwaysExits(Sub))
        return true;
    return false;
  }
  return false;
}

// Models the patch's index guard: `if (i >= K) break;` (also `i > K`,
// mirrored operands, or any unconditional early exit) means every statement
// after the guard only executes while i <= K - 1 (resp. i <= K), because
// those statements are dominated by the guard being false.
bool SAGenTestChecker::guardLimitsIndex(const IfStmt *IS, const VarDecl *V,
                                        ASTContext &Ctx, uint64_t &MaxIdxAfter) {
  if (!IS || !V)
    return false;
  if (!thenAlwaysExits(IS->getThen()))
    return false;

  const Expr *Cond = IS->getCond();
  if (!Cond)
    return false;
  const auto *BO = dyn_cast<BinaryOperator>(Cond->IgnoreParenImpCasts());
  if (!BO)
    return false;

  BinaryOperator::Opcode Op = BO->getOpcode();
  const Expr *LHS = BO->getLHS()->IgnoreParenImpCasts();
  const Expr *RHS = BO->getRHS()->IgnoreParenImpCasts();

  llvm::APSInt Limit;
  bool LoopVarOnLeft = false;
  if (indexIsLoopVar(LHS, V)) {
    if (!evalToInt(RHS, Limit, Ctx))
      return false;
    LoopVarOnLeft = true;
  } else if (indexIsLoopVar(RHS, V)) {
    if (!evalToInt(LHS, Limit, Ctx))
      return false;
  } else {
    return false;
  }
  if (Limit.isNegative())
    return false;

  // break/continue when i >= K  ->  later statements see i <= K - 1
  // break/continue when i > K   ->  later statements see i <= K
  BinaryOperator::Opcode AtLeast = LoopVarOnLeft ? BO_GE : BO_LE;
  BinaryOperator::Opcode Greater = LoopVarOnLeft ? BO_GT : BO_LT;
  if (Op == AtLeast) {
    uint64_t K = Limit.getZExtValue();
    if (K == 0)
      return false;
    MaxIdxAfter = K - 1;
    return true;
  }
  if (Op == Greater) {
    MaxIdxAfter = Limit.getZExtValue();
    return true;
  }
  return false;
}

// Collects loop-variable subscripts within one statement, each tagged with
// the index cap active at that statement.
void SAGenTestChecker::collectAccessesFlat(const Stmt *S, const VarDecl *V,
                                           uint64_t MaxIdx,
                                           llvm::SmallVector<SAGenTestChecker::AccessRecord, 8> &Out) {
  if (!S)
    return;
  class FlatVisitor : public RecursiveASTVisitor<FlatVisitor> {
    const VarDecl *V;
    uint64_t MaxIdx;
    llvm::SmallVector<SAGenTestChecker::AccessRecord, 8> &Out;

  public:
    FlatVisitor(const VarDecl *V, uint64_t MaxIdx,
                llvm::SmallVector<SAGenTestChecker::AccessRecord, 8> &Out)
        : V(V), MaxIdx(MaxIdx), Out(Out) {}

    bool VisitArraySubscriptExpr(ArraySubscriptExpr *ASE) {
      if (ASE && SAGenTestChecker::indexIsLoopVar(ASE->getIdx(), V))
        Out.push_back({ASE, MaxIdx});
      return true;
    }
  };
  FlatVisitor FV(V, MaxIdx, Out);
  FV.TraverseStmt(const_cast<Stmt *>(S));
}

// Order-sensitive walk of the loop body: a guard that unconditionally exits
// the loop tightens the maximum reachable index for every later statement.
void SAGenTestChecker::collectGuardedAccesses(const Stmt *S, const VarDecl *V,
                                              uint64_t MaxIdx, ASTContext &Ctx,
                                              llvm::SmallVector<SAGenTestChecker::AccessRecord, 8> &Out) {
  if (!S)
    return;

  if (const auto *CS = dyn_cast<CompoundStmt>(S)) {
    uint64_t Cur = MaxIdx;
    for (const Stmt *Sub : CS->body()) {
      collectGuardedAccesses(Sub, V, Cur, Ctx, Out);
      if (const auto *IS = dyn_cast<IfStmt>(Sub)) {
        uint64_t After;
        if (guardLimitsIndex(IS, V, Ctx, After) && After < Cur)
          Cur = After;
      }
    }
    return;
  }

  if (const auto *IS = dyn_cast<IfStmt>(S)) {
    // The condition is evaluated before any exit inside the branches.
    collectAccessesFlat(IS->getCond(), V, MaxIdx, Out);
    collectGuardedAccesses(IS->getThen(), V, MaxIdx, Ctx, Out);
    collectGuardedAccesses(IS->getElse(), V, MaxIdx, Ctx, Out);
    return;
  }

  if (const auto *LS = dyn_cast<LabelStmt>(S)) {
    collectGuardedAccesses(LS->getSubStmt(), V, MaxIdx, Ctx, Out);
    return;
  }

  collectAccessesFlat(S, V, MaxIdx, Out);
}

bool SAGenTestChecker::getArraySizeFromSubscriptBase(const Expr *Base, llvm::APInt &ArraySize, ASTContext &Ctx) {
  if (!Base)
    return false;

  // Case 1: direct DeclRefExpr to a variable with ConstantArrayType
  if (getArraySizeFromExpr(ArraySize, Base))
    return true;

  // Case 2: MemberExpr (struct or pointer-to-struct field)
  const MemberExpr *ME = dyn_cast<MemberExpr>(Base->IgnoreParenImpCasts());
  if (!ME) {
    // Try searching downward as a fallback
    ME = findSpecificTypeInChildren<MemberExpr>(Base);
  }
  if (ME) {
    if (const auto *FD = dyn_cast<FieldDecl>(ME->getMemberDecl())) {
      QualType T = FD->getType();
      if (const auto *CAT = dyn_cast<ConstantArrayType>(T.getTypePtr())) {
        ArraySize = CAT->getSize();
        return true;
      }
    }
  }

  // Unknown or pointer-based indexing: skip
  return false;
}

std::string SAGenTestChecker::getArrayName(const Expr *Base) {
  if (!Base)
    return std::string();

  Base = Base->IgnoreParenImpCasts();

  if (const auto *DRE = dyn_cast<DeclRefExpr>(Base)) {
    if (const auto *VD = dyn_cast<ValueDecl>(DRE->getDecl()))
      return VD->getNameAsString();
  }

  if (const auto *ME = dyn_cast<MemberExpr>(Base)) {
    if (const auto *VD = dyn_cast<ValueDecl>(ME->getMemberDecl()))
      return VD->getNameAsString();
  }

  // Fallback: try to find a nested MemberExpr
  if (const auto *ME2 = findSpecificTypeInChildren<MemberExpr>(Base)) {
    if (const auto *VD = dyn_cast<ValueDecl>(ME2->getMemberDecl()))
      return VD->getNameAsString();
  }

  return std::string();
}

void SAGenTestChecker::report(const ArraySubscriptExpr *ASE,
                              uint64_t BoundVal,
                              StringRef ArrName,
                              uint64_t ArrSize,
                              BugReporter &BR,
                              ASTContext &Ctx) const {
  if (!ASE)
    return;

  SmallString<128> Msg;
  llvm::raw_svector_ostream OS(Msg);
  OS << "Loop bound " << BoundVal << " exceeds array '" << ArrName
     << "' size " << ArrSize << "; " << ArrName << "[i] may be out of bounds";

  PathDiagnosticLocation Loc(ASE->getBeginLoc(), BR.getSourceManager());

  auto R = std::make_unique<BasicBugReport>(*BT, OS.str(), Loc);
  R->addRange(ASE->getSourceRange());
  BR.emitReport(std::move(R));
}

//============================ Main AST Callback =============================//

void SAGenTestChecker::checkASTCodeBody(const Decl *D, AnalysisManager &Mgr, BugReporter &BR) const {
  if (!D)
    return;
  const Stmt *Body = D->getBody();
  if (!Body)
    return;

  ASTContext &Ctx = Mgr.getASTContext();

  // Visitor to find ForStmt and analyze them.
  class Visitor : public RecursiveASTVisitor<Visitor> {
    const SAGenTestChecker *Checker;
    BugReporter &BR;
    ASTContext &Ctx;

  public:
    Visitor(const SAGenTestChecker *Checker, BugReporter &BR, ASTContext &Ctx)
        : Checker(Checker), BR(BR), Ctx(Ctx) {}

    bool VisitForStmt(const ForStmt *FS) {
      const VarDecl *LoopVar = nullptr;
      const Expr *BoundExpr = nullptr;
      bool IsStrictLess = true;

      if (!SAGenTestChecker::getCanonicalLoop(FS, LoopVar, BoundExpr, IsStrictLess, Ctx))
        return true;

      llvm::APSInt BoundAPS;
      if (!SAGenTestChecker::evalToInt(BoundExpr->IgnoreParenImpCasts(), BoundAPS, Ctx))
        return true;

      uint64_t BoundVal = BoundAPS.isSigned() ? static_cast<uint64_t>(BoundAPS.getExtValue()) : BoundAPS.getZExtValue();
      // We only handle non-negative bounds
      if ((BoundAPS.isSigned() && BoundAPS.isNegative()))
        return true;

      // Largest index the loop condition alone can reach:
      // `i < Bound` reaches Bound - 1; `i <= Bound` reaches Bound.
      uint64_t InitialMax = (IsStrictLess && BoundVal > 0) ? (BoundVal - 1) : BoundVal;

      // Collect subscripts indexed by the loop variable, each tagged with the
      // largest index reachable at that statement after applying every guard
      // that exits the loop before it.
      llvm::SmallVector<AccessRecord, 8> Accesses;
      if (const Stmt *LoopBody = FS->getBody())
        SAGenTestChecker::collectGuardedAccesses(LoopBody, LoopVar, InitialMax, Ctx, Accesses);

      // Report per array per loop (avoid duplicates)
      llvm::SmallPtrSet<const ValueDecl *, 8> Reported;

      for (const auto &Access : Accesses) {
        const ArraySubscriptExpr *ASE = Access.first;
        uint64_t MaxIdx = Access.second;
        if (!ASE)
          continue;

        llvm::APInt ArrSizeAP;
        if (!SAGenTestChecker::getArraySizeFromSubscriptBase(ASE->getBase(), ArrSizeAP, Ctx))
          continue;

        uint64_t ArrSize = ArrSizeAP.getLimitedValue(UINT64_MAX);

        // Out of bounds when the largest index reachable at this statement,
        // after every loop-exiting guard that dominates it, reaches the
        // array capacity.
        bool IsBug = (MaxIdx >= ArrSize);

        if (!IsBug)
          continue;

        // Identify the array's ValueDecl to deduplicate
        const ValueDecl *VDKey = nullptr;
        const Expr *Base = ASE->getBase()->IgnoreParenImpCasts();
        if (const auto *DRE = dyn_cast<DeclRefExpr>(Base)) {
          VDKey = dyn_cast<ValueDecl>(DRE->getDecl());
        } else if (const auto *ME = dyn_cast<MemberExpr>(Base)) {
          VDKey = dyn_cast<ValueDecl>(ME->getMemberDecl());
        } else if (const auto *ME2 = findSpecificTypeInChildren<MemberExpr>(ASE->getBase())) {
          VDKey = dyn_cast<ValueDecl>(ME2->getMemberDecl());
        }

        if (VDKey && Reported.contains(VDKey))
          continue;
        if (VDKey)
          Reported.insert(VDKey);

        std::string Name = SAGenTestChecker::getArrayName(ASE->getBase());
        Checker->report(ASE, BoundVal, Name.empty() ? StringRef("array") : StringRef(Name),
                        ArrSize, BR, Ctx);
      }

      return true;
    }
  };

  Visitor V(this, BR, Ctx);
  V.TraverseStmt(const_cast<Stmt *>(Body));
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detect loops that index into a smaller parallel array using a larger loop bound",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
