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
#include "llvm/ADT/ImmutableSet.h"
#include <memory>
#include <initializer_list>

using namespace clang;
using namespace ento;
using namespace taint;

//====================== Program state ======================

using LockSet = llvm::ImmutableSet<const MemRegion *>;
using ObjSet  = llvm::ImmutableSet<const MemRegion *>;

// Locks currently held on the path, and objects accessed while any tracked
// lock was held (i.e., data protected by a critical section).
REGISTER_TRAIT_WITH_PROGRAMSTATE(HeldLocks, LockSet)
REGISTER_TRAIT_WITH_PROGRAMSTATE(ProtectedObjs, ObjSet)

//====================== Helpers ============================

// The spinlock family supplies the critical-section state transition: an
// acquire call marks the start of a critical section on the given lock
// region, a release call ends it.  Kernel headers rewrite
// spin_lock_irqsave()/spin_unlock_irqrestore() into raw_spin_* and
// _raw_spin_* call targets with the identical lock-state contract, so the
// acquire/release roles are identified across the whole family rather than
// by a single exact spelling; helper setup APIs (init/trylock) are excluded.
static bool isLockAcquire(const CallEvent &Call) {
  const IdentifierInfo *ID = Call.getCalleeIdentifier();
  if (!ID)
    return false;
  StringRef N = ID->getName();
  return N.contains("spin_lock") && !N.contains("init") &&
         !N.contains("trylock");
}

static bool isLockRelease(const CallEvent &Call) {
  const IdentifierInfo *ID = Call.getCalleeIdentifier();
  if (!ID)
    return false;
  return ID->getName().contains("spin_unlock");
}

static const MemRegion *getLockRegionFromCall(const CallEvent &Call) {
  if (Call.getNumArgs() < 1)
    return nullptr;
  const MemRegion *R = Call.getArgSVal(0).getAsRegion();
  if (!R)
    return nullptr;
  return R->getBaseRegion();
}

static ProgramStateRef addHeldLock(ProgramStateRef State, const MemRegion *Lock) {
  if (!Lock) return State;
  LockSet Cur = State->get<HeldLocks>();
  LockSet::Factory &F = State->get_context<HeldLocks>();
  Cur = F.add(Cur, Lock);
  return State->set<HeldLocks>(Cur);
}

static ProgramStateRef removeHeldLock(ProgramStateRef State, const MemRegion *Lock) {
  if (!Lock) return State;
  LockSet Cur = State->get<HeldLocks>();
  LockSet::Factory &F = State->get_context<HeldLocks>();
  Cur = F.remove(Cur, Lock);
  State = State->set<HeldLocks>(Cur);
  return State;
}

static bool objSetContains(const ObjSet &S, const MemRegion *R) {
  for (auto It = S.begin(); It != S.end(); ++It) {
    if (*It == R)
      return true;
  }
  return false;
}

// Record that "ObjR" was accessed while a tracked lock is held.  The object
// counts as lock-protected data for the rest of the path, so a later write
// performed with no lock held is still attributed to that protection
// relation, independent of statement adjacency after the release.
static ProgramStateRef recordProtectedObject(ProgramStateRef State, const MemRegion *ObjR) {
  if (!ObjR)
    return State;
  if (State->get<HeldLocks>().isEmpty())
    return State;
  ObjSet Cur = State->get<ProtectedObjs>();
  if (objSetContains(Cur, ObjR))
    return State;
  ObjSet::Factory &F = State->get_context<ProtectedObjs>();
  Cur = F.add(Cur, ObjR);
  return State->set<ProtectedObjs>(Cur);
}

static bool isZeroSVal(SVal V) {
  if (auto CI = V.getAs<nonloc::ConcreteInt>())
    return CI->getValue() == 0;
  if (auto LCI = V.getAs<loc::ConcreteInt>())
    return LCI->getValue() == 0;
  return false;
}

// A store "obj->field = NULL" where field has pointer type: the structural
// shape of the patched site (identified by store semantics, not by name or
// source position).
static bool isNullPointerFieldStore(const Stmt *S, SVal Val, CheckerContext &C,
                                    const BinaryOperator *&OutBO) {
  OutBO = nullptr;

  const auto *BO = dyn_cast_or_null<BinaryOperator>(S);
  if (!BO || !BO->isAssignmentOp())
    return false;

  const Expr *LHS = BO->getLHS()->IgnoreParenCasts();
  const auto *ME = dyn_cast<MemberExpr>(LHS);
  if (!ME)
    return false;

  // We focus on pointer fields like obj->field.
  if (!ME->isArrow())
    return false;

  QualType FieldTy = ME->getType();
  if (FieldTy.isNull() || !FieldTy->isPointerType())
    return false;

  if (isZeroSVal(Val)) {
    OutBO = BO;
    return true;
  }

  Expr::EvalResult ER;
  if (BO->getRHS() && BO->getRHS()->EvaluateAsInt(ER, C.getASTContext()) &&
      ER.Val.isInt() && ER.Val.getInt() == 0) {
    OutBO = BO;
    return true;
  }
  return false;
}

//====================== Checker ============================

namespace {

class SAGenTestChecker
  : public Checker<check::BeginFunction,
                   check::PreCall,
                   check::PostCall,
                   check::Location,
                   check::Bind> {
   mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker() : BT(new BugType(this, "Pointer field NULLed after unlock", "Concurrency")) {}

  void checkBeginFunction(CheckerContext &C) const;
  void checkPreCall(const CallEvent &Call, CheckerContext &C) const;
  void checkPostCall(const CallEvent &Call, CheckerContext &C) const;
  void checkLocation(SVal Loc, bool IsLoad, const Stmt *S, CheckerContext &C) const;
  void checkBind(SVal Loc, SVal Val, const Stmt *S, CheckerContext &C) const;

private:
  void reportIssue(const BinaryOperator *BO, CheckerContext &C) const;
};

void SAGenTestChecker::checkBeginFunction(CheckerContext &C) const {
  // Reset per-path lock state only for a top-level function; entering an
  // inlined callee must not drop the caller's held locks.
  if (!C.getStackFrame()->inTopFrame())
    return;

  ProgramStateRef State = C.getState();
  State = State->set<HeldLocks>(State->get_context<HeldLocks>().getEmptySet());
  State = State->set<ProtectedObjs>(State->get_context<ProtectedObjs>().getEmptySet());
  C.addTransition(State);
}

void SAGenTestChecker::checkPreCall(const CallEvent &Call, CheckerContext &C) const {
  ProgramStateRef State = C.getState();

  // Handle lock acquire: a new critical section starts.
  if (isLockAcquire(Call)) {
    if (const MemRegion *LockR = getLockRegionFromCall(Call)) {
      State = addHeldLock(State, LockR);
    }
    C.addTransition(State);
    return;
  }

  if (isLockRelease(Call))
    return; // handled in checkPostCall once the release has taken effect

  // While a lock is held, pointer arguments passed to callees denote
  // objects that belong to the critical section.
  if (!State->get<HeldLocks>().isEmpty()) {
    for (unsigned i = 0; i < Call.getNumArgs(); ++i) {
      const MemRegion *ObjR = Call.getArgSVal(i).getAsRegion();
      if (ObjR)
        State = recordProtectedObject(State, ObjR->getBaseRegion());
    }
    C.addTransition(State);
  }
}

void SAGenTestChecker::checkPostCall(const CallEvent &Call, CheckerContext &C) const {
  ProgramStateRef State = C.getState();

  // Handle lock release: the critical section ends here.  Objects already
  // recorded as accessed under the lock stay marked as protected data.
  if (isLockRelease(Call)) {
    const MemRegion *LockR = getLockRegionFromCall(Call);
    State = removeHeldLock(State, LockR);
    C.addTransition(State);
  }
}

void SAGenTestChecker::checkLocation(SVal Loc, bool IsLoad, const Stmt *S, CheckerContext &C) const {
  ProgramStateRef State = C.getState();
  if (State->get<HeldLocks>().isEmpty())
    return;

  // Any region accessed while a lock is held belongs to the data guarded by
  // that critical section; remember its base object for the later,
  // post-release check.
  const MemRegion *R = Loc.getAsRegion();
  if (!R)
    return;

  ProgramStateRef NewState = recordProtectedObject(State, R->getBaseRegion());
  if (NewState != State)
    C.addTransition(NewState);
}

void SAGenTestChecker::checkBind(SVal Loc, SVal Val, const Stmt *S, CheckerContext &C) const {
  ProgramStateRef State = C.getState();

  const BinaryOperator *BO = nullptr;
  if (!isNullPointerFieldStore(S, Val, C, BO))
    return;

  // The NULL store is only racy when it happens with no tracked lock held.
  if (!State->get<HeldLocks>().isEmpty())
    return;

  // Objects whose data was accessed under a tracked lock stay recorded in
  // ProtectedObjs after the release: a pointer field NULLed outside the
  // critical section can race with readers that observed it under the lock.
  const MemRegion *R = Loc.getAsRegion();
  if (!R)
    return;
  const MemRegion *ObjR = R->getBaseRegion();

  if (!objSetContains(State->get<ProtectedObjs>(), ObjR))
    return;

  // Value relation at the sink: clearing a pointer field of lock-protected
  // data outside the critical section only races with locked readers when
  // the field may currently hold a live pointer.  A store that re-clears a
  // field already bound to NULL publishes no value change, so it is not a
  // state transition of the protection relation: once the clear happened
  // under the lock, any later redundant clear preserves that value.
  if (isZeroSVal(State->getSVal(R)))
    return;

  reportIssue(BO, C);
}

void SAGenTestChecker::reportIssue(const BinaryOperator *BO, CheckerContext &C) const {
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto R = std::make_unique<PathSensitiveBugReport>(
      *BT, "Pointer field set to NULL after unlocking; may race with readers under the lock", N);
  if (BO)
    R->addRange(BO->getLHS()->getSourceRange());
  C.emitReport(std::move(R));
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects setting lock-protected pointer fields to NULL immediately after unlocking",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
