// /external/upstream-study/SAGEN/result-0224-bugfail-multi-o3mini/test-Null-Pointer-Dereference-3027e7b15b02d2d37e3f82d6b8404f6d37e3b8cf/checkers/checker2.cpp
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

// Additional necessary includes
#include "clang/Lex/Lexer.h"
#include "clang/Basic/LangOptions.h"

using namespace clang;
using namespace ento;
using namespace taint;

//------------------------------------------------------------------------------
// Program state maps:
//
// DevmKasprintfReturnMap records the base region representing a return value
// originating from devm_kasprintf(). The bool is currently always true and
// exists because ProgramState maps are convenient for region provenance.
//
// ReportedDevmKasprintfReturnMap prevents repeated reports for the same
// allocation result along one analyzed path.
//------------------------------------------------------------------------------
REGISTER_MAP_WITH_PROGRAMSTATE(DevmKasprintfReturnMap, const MemRegion *, bool)
REGISTER_MAP_WITH_PROGRAMSTATE(ReportedDevmKasprintfReturnMap,
                               const MemRegion *, bool)

namespace {

static const MemRegion *getBaseRegion(SVal V) {
  const MemRegion *MR = V.getAsRegion();
  return MR ? MR->getBaseRegion() : nullptr;
}

static bool isDevmKasprintfCall(const CallEvent &Call) {
  const IdentifierInfo *II = Call.getCalleeIdentifier();
  return II && II->getName() == "devm_kasprintf";
}

static const MemRegion *getTrackedReturnRegion(ProgramStateRef State, SVal V) {
  const MemRegion *MR = getBaseRegion(V);
  if (!MR)
    return nullptr;

  const bool *Tracked = State->get<DevmKasprintfReturnMap>(MR);
  return Tracked && *Tracked ? MR : nullptr;
}

// A checked path must be constrained non-NULL. This relies on the analyzer's
// normal branch splitting, so `if (!ptr) return;` constrains ptr to non-NULL
// on the only path reaching subsequent uses.
static bool isTrackedAndMayBeNull(ProgramStateRef State, SVal V,
                                  const MemRegion *&TrackedMR) {
  TrackedMR = getTrackedReturnRegion(State, V);
  if (!TrackedMR)
    return false;

  return !State->isNull(V).isConstrainedFalse();
}

static bool isLocalPointerCarrier(const MemRegion *MR) {
  const auto *VR = dyn_cast_or_null<VarRegion>(MR);
  if (!VR)
    return false;

  const VarDecl *VD = VR->getDecl();
  if (!VD)
    return false;

  // An automatic local or parameter is only an alias carrier. A static local
  // persists beyond the scope and is treated as an escape.
  return VD->hasLocalStorage() && !VD->isStaticLocal();
}

// Return the pointer expression whose pointee is accessed by S. In
// particular, a plain DeclRefExpr such as `name` in `if (!name)` is not a
// dereference and deliberately returns nullptr.
static const Expr *getDereferencedPointerExpr(const Stmt *S) {
  const auto *E = dyn_cast_or_null<Expr>(S);
  if (!E)
    return nullptr;

  E = E->IgnoreParenImpCasts();

  if (const auto *UO = dyn_cast<UnaryOperator>(E)) {
    if (UO->getOpcode() == UO_Deref)
      return UO->getSubExpr()->IgnoreParenImpCasts();
  }

  if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(E))
    return ASE->getBase()->IgnoreParenImpCasts();

  if (const auto *ME = dyn_cast<MemberExpr>(E)) {
    if (ME->isArrow())
      return ME->getBase()->IgnoreParenImpCasts();
  }

  return nullptr;
}

class SAGenTestChecker : public Checker<
    check::PostCall,
    check::PreCall,
    check::Bind,
    check::Location> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Unchecked devm_kasprintf return",
                       "Null Dereference")) {}

  void checkPostCall(const CallEvent &Call, CheckerContext &C) const;
  void checkPreCall(const CallEvent &Call, CheckerContext &C) const;
  void checkBind(SVal Loc, SVal Val, const Stmt *StoreE,
                 CheckerContext &C) const;
  void checkLocation(SVal Loc, bool IsLoad, const Stmt *S,
                     CheckerContext &C) const;

private:
  void reportUncheckedUse(const MemRegion *TrackedMR, const Stmt *S,
                          CheckerContext &C) const;
};

void SAGenTestChecker::checkPostCall(const CallEvent &Call,
                                     CheckerContext &C) const {
  if (!isDevmKasprintfCall(Call))
    return;

  const MemRegion *MR = getBaseRegion(Call.getReturnValue());
  if (!MR)
    return;

  ProgramStateRef State = C.getState();
  State = State->set<DevmKasprintfReturnMap>(MR, true);
  C.addTransition(State);
}

void SAGenTestChecker::checkPreCall(const CallEvent &Call,
                                    CheckerContext &C) const {
  llvm::SmallVector<unsigned, 4> DerefParams;
  if (!functionKnownToDeref(Call, DerefParams))
    return;

  ProgramStateRef State = C.getState();

  for (unsigned ParamIndex : DerefParams) {
    if (ParamIndex >= Call.getNumArgs())
      continue;

    SVal Arg = Call.getArgSVal(ParamIndex);
    const MemRegion *TrackedMR = nullptr;
    if (!isTrackedAndMayBeNull(State, Arg, TrackedMR))
      continue;

    reportUncheckedUse(TrackedMR, Call.getOriginExpr(), C);
    return;
  }
}

void SAGenTestChecker::checkBind(SVal Loc, SVal Val, const Stmt *StoreE,
                                 CheckerContext &C) const {
  ProgramStateRef State = C.getState();

  const MemRegion *TrackedMR = nullptr;
  if (!isTrackedAndMayBeNull(State, Val, TrackedMR))
    return;

  const MemRegion *LHSRegion = Loc.getAsRegion();
  if (!LHSRegion)
    return;

  // `name = devm_kasprintf(...)` and `alias = name` only create local aliases.
  // Do not associate the unchecked state with the local VarRegion, because a
  // later read of that variable for `if (!name)` is not a pointer dereference.
  if (isLocalPointerCarrier(LHSRegion))
    return;

  // Storing into a field, global, static local, or indirect destination is an
  // escape. This catches the target code's `aux_driver->name = name` and
  // `aux_dev->name = name` before any later consumer can dereference it.
  reportUncheckedUse(TrackedMR, StoreE, C);
}

void SAGenTestChecker::checkLocation(SVal Loc, bool IsLoad, const Stmt *S,
                                     CheckerContext &C) const {
  const Expr *PointerExpr = getDereferencedPointerExpr(S);
  if (!PointerExpr)
    return;

  ProgramStateRef State = C.getState();
  SVal PointerVal =
      State->getSVal(PointerExpr, C.getLocationContext());

  const MemRegion *TrackedMR = nullptr;
  if (!isTrackedAndMayBeNull(State, PointerVal, TrackedMR))
    return;

  reportUncheckedUse(TrackedMR, S, C);
}

void SAGenTestChecker::reportUncheckedUse(const MemRegion *TrackedMR,
                                          const Stmt *S,
                                          CheckerContext &C) const {
  if (!TrackedMR || !S)
    return;

  ProgramStateRef State = C.getState();
  if (State->get<ReportedDevmKasprintfReturnMap>(TrackedMR))
    return;

  State = State->set<ReportedDevmKasprintfReturnMap>(TrackedMR, true);

  ExplodedNode *N = C.generateNonFatalErrorNode(State);
  if (!N)
    return;

  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT, "Unchecked devm_kasprintf return value used", N);
  Report->addRange(S->getSourceRange());
  C.emitReport(std::move(Report));
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects use of devm_kasprintf return value without NULL checking",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
