#include "clang/AST/Expr.h"
#include "clang/Basic/SourceLocation.h"
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
#include "llvm/ADT/ImmutableMap.h"
#include "llvm/ADT/StringRef.h"
#include <memory>

using namespace clang;
using namespace ento;

namespace {

struct CountOrigin {
  const MemRegion *Region;
  CountOrigin() : Region(nullptr) {}
  explicit CountOrigin(const MemRegion *R) : Region(R) {}

  bool operator==(const CountOrigin &Other) const { return Region == Other.Region; }
  void Profile(llvm::FoldingSetNodeID &ID) const { ID.AddPointer(Region); }
};

} // namespace

namespace llvm {
template <> struct FoldingSetTrait<CountOrigin> {
  static void Profile(const CountOrigin &X, FoldingSetNodeID &ID) { X.Profile(ID); }
};
} // namespace llvm

REGISTER_MAP_WITH_PROGRAMSTATE(TrackedCounts, const MemRegion *, CountOrigin)
REGISTER_SET_WITH_PROGRAMSTATE(GuardedCounts, const MemRegion *)

namespace {

static const Expr *ignoreCasts(const Expr *E) {
  return E ? E->IgnoreParenCasts() : nullptr;
}

static bool isNegativeIntegerLiteral(const Expr *E) {
  E = ignoreCasts(E);
  if (!E)
    return false;

  if (const auto *UO = dyn_cast<UnaryOperator>(E)) {
    if (UO->getOpcode() != UO_Minus)
      return false;
    const Expr *Sub = ignoreCasts(UO->getSubExpr());
    const auto *IL = dyn_cast_or_null<IntegerLiteral>(Sub);
    return IL && !IL->getValue().isZero();
  }

  return false;
}

static const MemRegion *getExprRegion(const Expr *E, CheckerContext &C) {
  E = ignoreCasts(E);
  if (!E)
    return nullptr;

  SVal V = C.getSVal(E);
  return V.getAsRegion();
}

static bool isTrackedRegion(const MemRegion *MR, ProgramStateRef State) {
  if (!MR)
    return false;
  const CountOrigin *Origin = State->get<TrackedCounts>(MR);
  return Origin && Origin->Region != nullptr;
}

static bool isGuardedRegion(const MemRegion *MR, ProgramStateRef State) {
  if (!MR)
    return false;
  return State->contains<GuardedCounts>(MR);
}

static ProgramStateRef markTracked(ProgramStateRef State, const MemRegion *MR) {
  if (!MR)
    return State;
  return State->set<TrackedCounts>(MR, CountOrigin(MR));
}

static ProgramStateRef markGuarded(ProgramStateRef State, const MemRegion *MR) {
  if (!MR)
    return State;
  return State->add<GuardedCounts>(MR);
}

static bool exprReferencesTrackedRegion(const Expr *E, CheckerContext &C,
                                        ProgramStateRef State,
                                        const MemRegion **Found) {
  E = ignoreCasts(E);
  if (!E)
    return false;

  if (const MemRegion *MR = getExprRegion(E, C)) {
    if (isTrackedRegion(MR, State)) {
      if (Found)
        *Found = MR;
      return true;
    }
  }

  for (const Stmt *Child : E->children()) {
    const auto *ChildExpr = dyn_cast_or_null<Expr>(Child);
    if (!ChildExpr)
      continue;
    if (exprReferencesTrackedRegion(ChildExpr, C, State, Found))
      return true;
  }

  return false;
}

static bool isNegativeGuardAgainstTracked(const Expr *Cond, CheckerContext &C,
                                          ProgramStateRef State,
                                          const MemRegion **Guarded) {
  Cond = ignoreCasts(Cond);
  if (!Cond)
    return false;

  if (const auto *UO = dyn_cast<UnaryOperator>(Cond)) {
    if (UO->getOpcode() == UO_LNot)
      return isNegativeGuardAgainstTracked(UO->getSubExpr(), C, State, Guarded);
  }

  const auto *BO = dyn_cast<BinaryOperator>(Cond);
  if (!BO || !BO->isComparisonOp())
    return false;

  const Expr *LHS = ignoreCasts(BO->getLHS());
  const Expr *RHS = ignoreCasts(BO->getRHS());
  if (!LHS || !RHS)
    return false;

  const MemRegion *MR = nullptr;

  switch (BO->getOpcode()) {
  case BO_LT:
  case BO_LE:
    if (isNegativeIntegerLiteral(RHS) || isNegativeIntegerLiteral(LHS))
      return false;
    if (const auto *IL = dyn_cast<IntegerLiteral>(RHS)) {
      if (IL->getValue().isZero() && exprReferencesTrackedRegion(LHS, C, State, &MR)) {
        if (Guarded)
          *Guarded = MR;
        return true;
      }
    }
    break;
  case BO_GT:
  case BO_GE:
    if (isNegativeIntegerLiteral(RHS) || isNegativeIntegerLiteral(LHS))
      return false;
    if (const auto *IL = dyn_cast<IntegerLiteral>(LHS)) {
      if (IL->getValue().isZero() && exprReferencesTrackedRegion(RHS, C, State, &MR)) {
        if (Guarded)
          *Guarded = MR;
        return true;
      }
    }
    break;
  default:
    break;
  }

  return false;
}

class UncheckedNegativeCountToSizeChecker
    : public Checker<check::PostCall, check::Bind, check::BranchCondition,
                     check::PreCall> {
  mutable std::unique_ptr<BugType> BT;

  bool isCountReturningFunction(const CallEvent &Call) const {
    const IdentifierInfo *II = Call.getCalleeIdentifier();
    if (!II)
      return false;

    StringRef Name = II->getName();
    if (Name.contains("canonicalize") || Name.contains("count") ||
        Name.contains("read") || Name.contains("reloc") ||
        Name.contains("symbol"))
      return true;

    return false;
  }

  bool isSizeConsumingSink(const CallEvent &Call, unsigned &CountArgIndex) const {
    const IdentifierInfo *II = Call.getCalleeIdentifier();
    if (!II)
      return false;

    StringRef Name = II->getName();
    if (Name == "qsort") {
      CountArgIndex = 1;
      return Call.getNumArgs() > CountArgIndex;
    }

    if (Name == "bsearch") {
      CountArgIndex = 2;
      return Call.getNumArgs() > CountArgIndex;
    }

    return false;
  }

public:
  UncheckedNegativeCountToSizeChecker()
      : BT(std::make_unique<BugType>(this,
                                     "Unchecked negative count reaches size sink",
                                     "Custom")) {}

  void checkPostCall(const CallEvent &Call, CheckerContext &C) const {
    if (!isCountReturningFunction(Call))
      return;

    const MemRegion *RetRegion = Call.getReturnValue().getAsRegion();
    if (!RetRegion)
      return;

    ProgramStateRef State = C.getState();
    State = markTracked(State, RetRegion);
    C.addTransition(State);
  }

  void checkBind(SVal Loc, SVal Val, const Stmt *S, CheckerContext &C) const {
    const MemRegion *Dst = Loc.getAsRegion();
    const MemRegion *Src = Val.getAsRegion();
    if (!Dst || !Src)
      return;

    ProgramStateRef State = C.getState();
    if (!isTrackedRegion(Src, State))
      return;

    State = markTracked(State, Dst);
    C.addTransition(State);
  }

  void checkBranchCondition(const Stmt *Condition, CheckerContext &C) const {
    const auto *Cond = dyn_cast_or_null<Expr>(Condition);
    if (!Cond)
      return;

    ProgramStateRef State = C.getState();
    const MemRegion *MR = nullptr;
    if (!isNegativeGuardAgainstTracked(Cond, C, State, &MR))
      return;
    if (!MR)
      return;

    State = markGuarded(State, MR);
    C.addTransition(State);
  }

  void checkPreCall(const CallEvent &Call, CheckerContext &C) const {
    unsigned CountArgIndex = 0;
    if (!isSizeConsumingSink(Call, CountArgIndex))
      return;

    const Expr *CountExpr = Call.getArgExpr(CountArgIndex);
    if (!CountExpr)
      return;

    ProgramStateRef State = C.getState();
    const MemRegion *MR = nullptr;
    if (!exprReferencesTrackedRegion(CountExpr, C, State, &MR))
      return;
    if (!MR || isGuardedRegion(MR, State))
      return;

    ExplodedNode *N = C.generateNonFatalErrorNode();
    if (!N)
      return;

    auto R = std::make_unique<PathSensitiveBugReport>(
        *BT,
        "A signed count that may encode a negative error value reaches a size-based API without a non-negative check.",
        N);
    R->addRange(CountExpr->getSourceRange());
    C.emitReport(std::move(R));
  }
};

} // namespace

extern "C" void clang_registerCheckers(CheckerRegistry &Registry) {
  Registry.addChecker<UncheckedNegativeCountToSizeChecker>(
      "custom.UncheckedNegativeCountToSizeChecker",
      "Detects signed count/error values reaching size-consuming APIs without non-negative validation.",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] = CLANG_VERSION_STRING;
