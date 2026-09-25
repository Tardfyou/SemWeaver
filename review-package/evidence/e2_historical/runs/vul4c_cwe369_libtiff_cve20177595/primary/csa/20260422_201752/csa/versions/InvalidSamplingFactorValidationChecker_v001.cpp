#include "clang/AST/Expr.h"
#include "clang/Basic/Version.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/CheckerManager.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/MemRegion.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramStateTrait.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SVals.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include <memory>

using namespace clang;
using namespace ento;

namespace {

struct TrackedSymbolKey {
  const MemRegion *Region;

  bool operator==(const TrackedSymbolKey &Other) const {
    return Region == Other.Region;
  }

  void Profile(llvm::FoldingSetNodeID &ID) const { ID.AddPointer(Region); }
};

} // namespace

REGISTER_SET_WITH_PROGRAMSTATE(TrackedSamplingRegions, const MemRegion *)
REGISTER_SET_WITH_PROGRAMSTATE(ZeroGuardedSamplingRegions, const MemRegion *)

namespace {

static bool isZeroIntegerLiteral(const Expr *E) {
  E = E ? E->IgnoreParenCasts() : nullptr;
  if (const auto *IL = dyn_cast_or_null<IntegerLiteral>(E))
    return IL->getValue().isZero();
  return false;
}

static llvm::StringRef getMemberName(const MemberExpr *ME) {
  if (!ME)
    return llvm::StringRef();
  const ValueDecl *VD = ME->getMemberDecl();
  if (!VD)
    return llvm::StringRef();
  return VD->getName();
}

static bool nameLooksRisky(llvm::StringRef Name) {
  return Name.contains("sampling") || Name.contains("subsampling") ||
         Name.contains("length") || Name.contains("size") ||
         Name.contains("width") || Name.contains("height") ||
         Name.contains("stride");
}

static bool exprLooksLikeRiskySource(const Expr *E) {
  E = E ? E->IgnoreParenCasts() : nullptr;
  if (!E)
    return false;

  if (const auto *ME = dyn_cast<MemberExpr>(E))
    return nameLooksRisky(getMemberName(ME));

  if (const auto *DRE = dyn_cast<DeclRefExpr>(E)) {
    const ValueDecl *VD = DRE->getDecl();
    if (!VD)
      return false;
    return nameLooksRisky(VD->getName());
  }

  if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(E))
    return exprLooksLikeRiskySource(ASE->getBase()) ||
           exprLooksLikeRiskySource(ASE->getIdx());

  if (const auto *BO = dyn_cast<BinaryOperator>(E))
    return exprLooksLikeRiskySource(BO->getLHS()) ||
           exprLooksLikeRiskySource(BO->getRHS());

  if (const auto *UO = dyn_cast<UnaryOperator>(E))
    return exprLooksLikeRiskySource(UO->getSubExpr());

  if (const auto *CE = dyn_cast<CallExpr>(E)) {
    for (const Expr *Arg : CE->arguments()) {
      if (exprLooksLikeRiskySource(Arg))
        return true;
    }
  }

  return false;
}

static const MemRegion *getRegionFromExprSVal(const Expr *E, CheckerContext &C) {
  if (!E)
    return nullptr;
  SVal V = C.getSVal(E);
  return V.getAsRegion();
}

static bool isRiskyAssignment(const BinaryOperator *BO) {
  if (!BO || !BO->isAssignmentOp())
    return false;
  const Expr *LHS = BO->getLHS();
  const Expr *RHS = BO->getRHS();
  if (!LHS || !RHS)
    return false;

  LHS = LHS->IgnoreParenCasts();
  if (const auto *ME = dyn_cast<MemberExpr>(LHS)) {
    if (nameLooksRisky(getMemberName(ME)) && exprLooksLikeRiskySource(RHS))
      return true;
  }

  if (const auto *DRE = dyn_cast<DeclRefExpr>(LHS)) {
    const ValueDecl *VD = DRE->getDecl();
    if (VD && nameLooksRisky(VD->getName()) && exprLooksLikeRiskySource(RHS))
      return true;
  }

  return false;
}

static void collectReferencedRegions(const Expr *E, CheckerContext &C,
                                     llvm::SmallPtrSetImpl<const MemRegion *> &Out) {
  E = E ? E->IgnoreParenCasts() : nullptr;
  if (!E)
    return;

  if (const MemRegion *MR = getRegionFromExprSVal(E, C))
    Out.insert(MR);

  if (const auto *BO = dyn_cast<BinaryOperator>(E)) {
    collectReferencedRegions(BO->getLHS(), C, Out);
    collectReferencedRegions(BO->getRHS(), C, Out);
    return;
  }

  if (const auto *UO = dyn_cast<UnaryOperator>(E)) {
    collectReferencedRegions(UO->getSubExpr(), C, Out);
    return;
  }

  if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(E)) {
    collectReferencedRegions(ASE->getBase(), C, Out);
    collectReferencedRegions(ASE->getIdx(), C, Out);
    return;
  }

  if (const auto *ME = dyn_cast<MemberExpr>(E)) {
    collectReferencedRegions(ME->getBase(), C, Out);
    return;
  }

  if (const auto *CE = dyn_cast<CallExpr>(E)) {
    for (const Expr *Arg : CE->arguments())
      collectReferencedRegions(Arg, C, Out);
  }
}

static bool exprReferencesTrackedRegion(const Expr *E, CheckerContext &C,
                                        ProgramStateRef State) {
  llvm::SmallPtrSet<const MemRegion *, 8> Refs;
  collectReferencedRegions(E, C, Refs);
  for (const MemRegion *MR : Refs) {
    if (State->contains<TrackedSamplingRegions>(MR))
      return true;
  }
  return false;
}

static bool exprReferencesUnguardedTrackedRegion(const Expr *E, CheckerContext &C,
                                                 ProgramStateRef State) {
  llvm::SmallPtrSet<const MemRegion *, 8> Refs;
  collectReferencedRegions(E, C, Refs);
  for (const MemRegion *MR : Refs) {
    if (State->contains<TrackedSamplingRegions>(MR) &&
        !State->contains<ZeroGuardedSamplingRegions>(MR))
      return true;
  }
  return false;
}

static bool isZeroCheckForExpr(const Expr *Cond, const Expr *Target) {
  Cond = Cond ? Cond->IgnoreParenCasts() : nullptr;
  Target = Target ? Target->IgnoreParenCasts() : nullptr;
  if (!Cond || !Target)
    return false;

  if (const auto *UO = dyn_cast<UnaryOperator>(Cond)) {
    if (UO->getOpcode() == UO_LNot) {
      const Expr *Sub = UO->getSubExpr();
      Sub = Sub ? Sub->IgnoreParenCasts() : nullptr;
      return Sub == Target;
    }
  }

  const auto *BO = dyn_cast<BinaryOperator>(Cond);
  if (!BO)
    return false;

  BinaryOperatorKind Op = BO->getOpcode();
  const Expr *LHS = BO->getLHS() ? BO->getLHS()->IgnoreParenCasts() : nullptr;
  const Expr *RHS = BO->getRHS() ? BO->getRHS()->IgnoreParenCasts() : nullptr;
  if (!LHS || !RHS)
    return false;

  switch (Op) {
  case BO_EQ:
    return (LHS == Target && isZeroIntegerLiteral(RHS)) ||
           (RHS == Target && isZeroIntegerLiteral(LHS));
  case BO_LE:
    return LHS == Target && isZeroIntegerLiteral(RHS);
  case BO_LT:
    return LHS == Target &&
           RHS && RHS->IgnoreParenCasts() &&
           isa<IntegerLiteral>(RHS) &&
           cast<IntegerLiteral>(RHS)->getValue() == 1;
  case BO_LOr:
  case BO_LAnd:
    return isZeroCheckForExpr(BO->getLHS(), Target) ||
           isZeroCheckForExpr(BO->getRHS(), Target);
  default:
    return false;
  }
}

class InvalidSamplingFactorValidationChecker
    : public Checker<check::Bind, check::BranchCondition,
                     check::PreStmt<BinaryOperator>, check::PreCall> {
  mutable std::unique_ptr<BugType> BT;

public:
  InvalidSamplingFactorValidationChecker()
      : BT(std::make_unique<BugType>(this,
                                     "Invalid sampling factor validation",
                                     "Custom")) {}

  void checkBind(SVal Loc, SVal Val, const Stmt *S, CheckerContext &C) const {
    const auto *BO = dyn_cast_or_null<BinaryOperator>(S);
    if (!isRiskyAssignment(BO))
      return;

    const MemRegion *MR = Loc.getAsRegion();
    if (!MR)
      return;

    ProgramStateRef State = C.getState();
    State = State->add<TrackedSamplingRegions>(MR);
    C.addTransition(State);
  }

  void checkBranchCondition(const Stmt *Condition, CheckerContext &C) const {
    const Expr *Cond = dyn_cast_or_null<Expr>(Condition);
    if (!Cond)
      return;

    ProgramStateRef State = C.getState();
    llvm::SmallPtrSet<const MemRegion *, 8> Refs;
    collectReferencedRegions(Cond, C, Refs);

    bool Changed = false;
    for (const MemRegion *MR : Refs) {
      if (!MR)
        continue;
      if (!State->contains<TrackedSamplingRegions>(MR))
        continue;

      if (isZeroCheckForExpr(Cond, dyn_cast<Expr>(nullptr))) {
        continue;
      }
    }

    const auto *BO = dyn_cast<BinaryOperator>(Cond->IgnoreParenCasts());
    if (!BO)
      return;

    llvm::SmallVector<const Expr *, 8> Candidates;
    collectCandidateExprs(Cond->IgnoreParenCasts(), Candidates);

    for (const Expr *Candidate : Candidates) {
      const MemRegion *MR = getRegionFromExprSVal(Candidate, C);
      if (!MR)
        continue;
      if (!State->contains<TrackedSamplingRegions>(MR))
        continue;
      if (!isZeroCheckForExpr(Cond, Candidate))
        continue;
      State = State->add<ZeroGuardedSamplingRegions>(MR);
      Changed = true;
    }

    if (Changed)
      C.addTransition(State);
  }

  void checkPreStmt(const BinaryOperator *BO, CheckerContext &C) const {
    if (!BO)
      return;

    BinaryOperatorKind Op = BO->getOpcode();
    if (Op != BO_Div && Op != BO_Rem && Op != BO_DivAssign && Op != BO_RemAssign)
      return;

    const Expr *RHS = BO->getRHS();
    if (!RHS)
      return;

    ProgramStateRef State = C.getState();
    if (!exprReferencesUnguardedTrackedRegion(RHS, C, State))
      return;

    ExplodedNode *N = C.generateNonFatalErrorNode();
    if (!N)
      return;

    auto R = std::make_unique<PathSensitiveBugReport>(
        *BT,
        "Division or remainder uses sampling/size-like value derived from metadata without a prior zero-value rejection guard.",
        N);
    R->addRange(RHS->getSourceRange());
    C.emitReport(std::move(R));
  }

  void checkPreCall(const CallEvent &Call, CheckerContext &C) const {
    ProgramStateRef State = C.getState();
    const IdentifierInfo *II = Call.getCalleeIdentifier();
    llvm::StringRef FuncName = II ? II->getName() : llvm::StringRef();

    bool SuspiciousConsumer = false;
    if (FuncName.contains("JPEG") || FuncName.contains("Decode") ||
        FuncName.contains("Encode") || FuncName.contains("Setup") ||
        FuncName.contains("Tile") || FuncName.contains("Strip")) {
      SuspiciousConsumer = true;
    }

    if (!SuspiciousConsumer)
      return;

    for (unsigned I = 0; I < Call.getNumArgs(); ++I) {
      const Expr *Arg = Call.getArgExpr(I);
      if (!Arg)
        continue;
      if (!exprReferencesUnguardedTrackedRegion(Arg, C, State))
        continue;

      ExplodedNode *N = C.generateNonFatalErrorNode();
      if (!N)
        return;

      auto R = std::make_unique<PathSensitiveBugReport>(
          *BT,
          "Metadata-derived sampling/size parameter reaches a codec/layout consumer without a prior zero-value rejection guard.",
          N);
      R->addRange(Arg->getSourceRange());
      C.emitReport(std::move(R));
      return;
    }
  }

private:
  static void collectCandidateExprs(const Expr *E,
                                    llvm::SmallVectorImpl<const Expr *> &Out) {
    E = E ? E->IgnoreParenCasts() : nullptr;
    if (!E)
      return;

    if (isa<DeclRefExpr>(E) || isa<MemberExpr>(E) || isa<ArraySubscriptExpr>(E))
      Out.push_back(E);

    if (const auto *BO = dyn_cast<BinaryOperator>(E)) {
      collectCandidateExprs(BO->getLHS(), Out);
      collectCandidateExprs(BO->getRHS(), Out);
      return;
    }

    if (const auto *UO = dyn_cast<UnaryOperator>(E)) {
      collectCandidateExprs(UO->getSubExpr(), Out);
      return;
    }

    if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(E)) {
      collectCandidateExprs(ASE->getBase(), Out);
      collectCandidateExprs(ASE->getIdx(), Out);
      return;
    }

    if (const auto *ME = dyn_cast<MemberExpr>(E)) {
      collectCandidateExprs(ME->getBase(), Out);
      return;
    }
  }
};

} // namespace

extern "C" void clang_registerCheckers(CheckerRegistry &Registry) {
  Registry.addChecker<InvalidSamplingFactorValidationChecker>(
      "custom.InvalidSamplingFactorValidationChecker",
      "Detects metadata-derived sampling or size factors used without non-zero validation.",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] = CLANG_VERSION_STRING;