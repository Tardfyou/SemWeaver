// /external/upstream-study/SAGEN/result-0224-bugfail-multi-o3mini/test-Double-Free-d8c561741ef83980114b3f7f95ffac54600f3f16/checkers/checker2.cpp
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
#include "clang/AST/Expr.h"  // for Expr

using namespace clang;
using namespace ento;
using namespace taint;

REGISTER_MAP_WITH_PROGRAMSTATE(PendingSqCreate, const MemRegion *, SymbolRef);
REGISTER_MAP_WITH_PROGRAMSTATE(PendingSqReady, const MemRegion *, SymbolRef);
REGISTER_SET_WITH_PROGRAMSTATE(CreatedSq, const MemRegion *);

namespace {

static bool isNamedCall(const CallEvent &Call, llvm::StringRef Name) {
  const auto *FD = dyn_cast_or_null<FunctionDecl>(Call.getDecl());
  return FD && FD->getName() == Name;
}

static const MemRegion *getMemberObjectRegion(const Expr *E,
                                               CheckerContext &C) {
  E = E->IgnoreParenImpCasts();
  const auto *Member = dyn_cast<MemberExpr>(E);
  if (!Member)
    return nullptr;
  return C.getSVal(Member->getBase()).getAsRegion();
}

static const MemRegion *getOutputObjectRegion(const Expr *E,
                                               CheckerContext &C) {
  E = E->IgnoreParenImpCasts();
  const auto *Address = dyn_cast<UnaryOperator>(E);
  if (!Address || Address->getOpcode() != UO_AddrOf)
    return nullptr;
  return getMemberObjectRegion(Address->getSubExpr(), C);
}

static const MemRegion *getArgumentObjectRegion(const Expr *E,
                                                 CheckerContext &C) {
  return C.getSVal(E->IgnoreParenImpCasts()).getAsRegion();
}

static bool isConstrainedZero(ProgramStateRef State, SymbolRef Status) {
  const llvm::APSInt *Value =
      State->getConstraintManager().getSymVal(State, Status);
  return Value && Value->isZero();
}

static bool isConstrainedNonzero(ProgramStateRef State, SymbolRef Status) {
  const llvm::APSInt *Value =
      State->getConstraintManager().getSymVal(State, Status);
  return Value && !Value->isZero();
}

// The core create call owns an SQ only after its status is known to be zero.
// A subsequent ready failure must release that core SQ without consuming the
// caller-owned host buffers in the enclosing error flow.
class SAGenTestChecker : public Checker<check::PreCall, eval::Assume> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Double-free after SQ readiness failure")) {}

  void checkPreCall(const CallEvent &Call, CheckerContext &C) const;
  ProgramStateRef evalAssume(ProgramStateRef State, SVal Cond,
                             bool Assumption) const;
};

ProgramStateRef SAGenTestChecker::evalAssume(ProgramStateRef State, SVal Cond,
                                             bool Assumption) const {
  (void)Cond;
  (void)Assumption;

  const auto &Creates = State->get<PendingSqCreate>();
  for (const auto &Entry : Creates) {
    if (!isConstrainedZero(State, Entry.second))
      continue;
    State = State->remove<PendingSqCreate>(Entry.first);
    State = State->add<CreatedSq>(Entry.first);
  }

  const auto &Readies = State->get<PendingSqReady>();
  for (const auto &Entry : Readies) {
    if (isConstrainedZero(State, Entry.second))
      State = State->remove<PendingSqReady>(Entry.first);
  }

  return State;
}

void SAGenTestChecker::checkPreCall(const CallEvent &Call,
                                    CheckerContext &C) const {
  ProgramStateRef State = C.getState();

  if (isNamedCall(Call, "mlx5_core_create_sq")) {
    if (Call.getNumArgs() < 4)
      return;
    const MemRegion *Sq = getOutputObjectRegion(Call.getArgExpr(3), C);
    SymbolRef Status = Call.getReturnValue().getAsSymbol();
    if (!Sq || !Status)
      return;
    C.addTransition(State->set<PendingSqCreate>(Sq, Status));
    return;
  }

  if (isNamedCall(Call, "hws_send_ring_set_sq_rdy")) {
    if (Call.getNumArgs() < 2)
      return;
    const MemRegion *Sq = getMemberObjectRegion(Call.getArgExpr(1), C);
    SymbolRef Status = Call.getReturnValue().getAsSymbol();
    if (!Sq || !Status)
      return;
    const auto &Created = State->get<CreatedSq>();
    if (!Created.contains(Sq))
      return;
    C.addTransition(State->set<PendingSqReady>(Sq, Status));
    return;
  }

  if (isNamedCall(Call, "mlx5_core_destroy_sq")) {
    if (Call.getNumArgs() < 2)
      return;
    const MemRegion *Sq = getMemberObjectRegion(Call.getArgExpr(1), C);
    if (!Sq)
      return;
    State = State->remove<CreatedSq>(Sq);
    State = State->remove<PendingSqReady>(Sq);
    C.addTransition(State);
    return;
  }

  if (!isNamedCall(Call, "hws_send_ring_close_sq") ||
      Call.getNumArgs() < 1)
    return;

  const MemRegion *Sq = getArgumentObjectRegion(Call.getArgExpr(0), C);
  if (!Sq)
    return;

  const auto &Readies = State->get<PendingSqReady>();
  auto Ready = Readies.find(Sq);
  if (Ready == Readies.end() ||
      !isConstrainedNonzero(State, Ready->second))
    return;

  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT,
      "Full SQ cleanup after readiness failure can release caller-owned buffers twice",
      N);
  Report->addRange(Call.getSourceRange());
  C.emitReport(std::move(Report));
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker", 
      "Detects full SQ cleanup after readiness failure while core SQ ownership is live", 
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
