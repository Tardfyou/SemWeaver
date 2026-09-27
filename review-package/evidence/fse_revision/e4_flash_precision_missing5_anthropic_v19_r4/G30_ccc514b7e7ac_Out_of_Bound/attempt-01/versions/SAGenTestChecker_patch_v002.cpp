#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include "clang/AST/RecursiveASTVisitor.h"
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

      // An access A[i] paired with the maximum value i can take at that point.
      struct CappedAccess {
        const ArraySubscriptExpr *ASE;
        uint64_t MaxIdx;
      };

      static bool isLoopTerminating(const Stmt *S);

      static bool matchIndexGuard(const Expr *Cond, const VarDecl *V,
                                  BinaryOperator::Opcode &Op, llvm::APSInt &K);

      static void collectAccesses(const Stmt *S, const VarDecl *V, uint64_t MaxIdx,
                                  llvm::SmallVectorImpl<CappedAccess> &Out);

      static void walkLoopBody(const Stmt *S, const VarDecl *V, uint64_t &MaxIdx,
                               llvm::SmallVectorImpl<CappedAccess> &Out);

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

// True when the statement unconditionally leaves the loop body.
bool SAGenTestChecker::isLoopTerminating(const Stmt *S) {
  if (!S)
    return false;
  if (isa<BreakStmt>(S) || isa<ContinueStmt>(S) || isa<ReturnStmt>(S))
    return true;
  if (const auto *CS = dyn_cast<CompoundStmt>(S))
    return !CS->body_empty() && isLoopTerminating(CS->body_back());
  if (const auto *If = dyn_cast<IfStmt>(S))
    return isLoopTerminating(If->getThen()) && isLoopTerminating(If->getElse());
  return false;
}

// Matches loop-index guards "i < K", "i <= K", "i > K", "i >= K" (either
// operand order, optionally negated). Returns the canonical opcode relative
// to the loop variable and the constant bound K.
bool SAGenTestChecker::matchIndexGuard(const Expr *Cond, const VarDecl *V,
                                       BinaryOperator::Opcode &Op,
                                       llvm::APSInt &KOut) {
  if (!Cond || !V)
    return false;

  const Expr *E = Cond->IgnoreParenImpCasts();
  bool Negated = false;
  if (const auto *UO = dyn_cast<UnaryOperator>(E)) {
    if (UO->getOpcode() == UO_LNot) {
      Negated = true;
      E = UO->getSubExpr()->IgnoreParenImpCasts();
    }
  }

  const auto *BO = dyn_cast<BinaryOperator>(E);
  if (!BO)
    return false;

  BinaryOperator::Opcode Raw = BO->getOpcode();
  if (Raw != BO_LT && Raw != BO_LE && Raw != BO_GT && Raw != BO_GE)
    return false;

  const Expr *LHS = BO->getLHS()->IgnoreParenImpCasts();
  const Expr *RHS = BO->getRHS()->IgnoreParenImpCasts();
  const auto *LDRE = dyn_cast<DeclRefExpr>(LHS);
  const auto *RDRE = dyn_cast<DeclRefExpr>(RHS);

  const Expr *KE = nullptr;
  if (LDRE && LDRE->getDecl() == V && !(RDRE && RDRE->getDecl() == V)) {
    KE = RHS;
  } else if (RDRE && RDRE->getDecl() == V && !(LDRE && LDRE->getDecl() == V)) {
    // Mirror "K <op> i" into "i <mirrored-op> K".
    KE = LHS;
    if (Raw == BO_LT)
      Raw = BO_GT;
    else if (Raw == BO_LE)
      Raw = BO_GE;
    else if (Raw == BO_GT)
      Raw = BO_LT;
    else
      Raw = BO_LE;
  } else {
    return false;
  }

  Expr::EvalResult ER;
  if (!KE->EvaluateAsInt(ER, V->getASTContext()))
    return false;
  llvm::APSInt K = ER.Val.getInt();
  if (K.isSigned() && K.isNegative())
    return false;

  if (Negated) {
    // "!(i < K)" is "i >= K", and likewise for the other comparisons.
    if (Raw == BO_LT)
      Raw = BO_GE;
    else if (Raw == BO_LE)
      Raw = BO_GT;
    else if (Raw == BO_GE)
      Raw = BO_LT;
    else
      Raw = BO_LE;
  }

  Op = Raw;
  KOut = K;
  return true;
}

// Collect A[i] accesses whose index is the loop variable, tagging each with
// the caller-supplied maximum index value.
void SAGenTestChecker::collectAccesses(const Stmt *S, const VarDecl *V,
                                       uint64_t MaxIdx,
                                       llvm::SmallVectorImpl<CappedAccess> &Out) {
  if (!S)
    return;
  class AccessCollector : public RecursiveASTVisitor<AccessCollector> {
    const VarDecl *V;
    uint64_t MaxIdx;
    llvm::SmallVectorImpl<CappedAccess> &Out;

  public:
    AccessCollector(const VarDecl *V, uint64_t MaxIdx,
                    llvm::SmallVectorImpl<CappedAccess> &Out)
        : V(V), MaxIdx(MaxIdx), Out(Out) {}

    bool VisitArraySubscriptExpr(const ArraySubscriptExpr *ASE) {
      if (ASE && indexIsLoopVar(ASE->getIdx(), V))
        Out.push_back({ASE, MaxIdx});
      return true;
    }
  };

  AccessCollector AC(V, MaxIdx, Out);
  AC.TraverseStmt(const_cast<Stmt *>(S));
}

// Walk the loop body in source order, lowering MaxIdx once a terminating
// guard "if (i >= K) break/continue/return;" dominates later statements.
void SAGenTestChecker::walkLoopBody(const Stmt *S, const VarDecl *V,
                                    uint64_t &MaxIdx,
                                    llvm::SmallVectorImpl<CappedAccess> &Out) {
  if (!S)
    return;

  if (const auto *CS = dyn_cast<CompoundStmt>(S)) {
    for (const Stmt *Child : CS->body())
      walkLoopBody(Child, V, MaxIdx, Out);
    return;
  }

  if (const auto *If = dyn_cast<IfStmt>(S)) {
    // Condition accesses execute before any branch is taken.
    collectAccesses(If->getCond(), V, MaxIdx, Out);

    BinaryOperator::Opcode GOp;
    llvm::APSInt GK;
    if (matchIndexGuard(If->getCond(), V, GOp, GK)) {
      uint64_t KVal = GK.getLimitedValue(UINT64_MAX);

      if ((GOp == BO_GE || GOp == BO_GT) && isLoopTerminating(If->getThen())) {
        // "if (i >= K) break/continue/return;" caps i below K on the
        // fall-through path: the else arm and all later statements.
        uint64_t Cap = (GOp == BO_GE) ? (KVal ? KVal - 1 : 0) : KVal;
        uint64_t ThenMax = MaxIdx;
        walkLoopBody(If->getThen(), V, ThenMax, Out);
        uint64_t Guarded = Cap < MaxIdx ? Cap : MaxIdx;
        uint64_t ElseMax = Guarded;
        walkLoopBody(If->getElse(), V, ElseMax, Out);
        MaxIdx = Guarded;
        return;
      }

      if (GOp == BO_LT || GOp == BO_LE) {
        // "if (i < K)" caps the index inside the then arm only.
        uint64_t Cap = (GOp == BO_LT) ? (KVal ? KVal - 1 : 0) : KVal;
        uint64_t ThenMax = Cap < MaxIdx ? Cap : MaxIdx;
        walkLoopBody(If->getThen(), V, ThenMax, Out);
        uint64_t ElseMax = MaxIdx;
        walkLoopBody(If->getElse(), V, ElseMax, Out);
        return;
      }
    }

    // Unrelated condition: both arms inherit the current maximum; caps
    // introduced inside an arm stay local to that arm.
    uint64_t ThenMax = MaxIdx;
    walkLoopBody(If->getThen(), V, ThenMax, Out);
    uint64_t ElseMax = MaxIdx;
    walkLoopBody(If->getElse(), V, ElseMax, Out);
    return;
  }

  // Any other statement: conservative collection with the current maximum;
  // guards inside nested constructs are not modeled here.
  collectAccesses(S, V, MaxIdx, Out);
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

      // Maximum index the loop condition alone allows.
      uint64_t LoopMaxIdx = IsStrictLess ? (BoundVal ? BoundVal - 1 : 0) : BoundVal;

      // Collect loop-var indexed accesses together with the maximum index
      // value that can reach each access, honoring in-loop index guards such
      // as "if (i >= K) break;", which cap the index on their fall-through
      // path. The guard's bound is a capacity relation: only when the capped
      // maximum still reaches the array capacity does the access overflow.
      llvm::SmallVector<CappedAccess, 8> Accesses;
      if (const Stmt *LoopBody = FS->getBody()) {
        uint64_t BodyMaxIdx = LoopMaxIdx;
        walkLoopBody(LoopBody, LoopVar, BodyMaxIdx, Accesses);
      }

      // Report per array per loop (avoid duplicates)
      llvm::SmallPtrSet<const ValueDecl *, 8> Reported;

      for (const CappedAccess &CA : Accesses) {
        const ArraySubscriptExpr *ASE = CA.ASE;
        if (!ASE)
          continue;

        llvm::APInt ArrSizeAP;
        if (!SAGenTestChecker::getArraySizeFromSubscriptBase(ASE->getBase(), ArrSizeAP, Ctx))
          continue;

        uint64_t ArrSize = ArrSizeAP.getLimitedValue(UINT64_MAX);

        // Out of bounds only if the guard-aware maximum index at this access
        // reaches or passes the array capacity; a dominating in-loop guard
        // lowers that maximum below the capacity, keeping guarded code silent.
        bool IsBug = (CA.MaxIdx >= ArrSize);

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
