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
#include "llvm/ADT/ImmutableMap.h"
#include <memory>
#include <initializer_list>

using namespace clang;
using namespace ento;
using namespace taint;

//====================== Program state ======================

using RegionMap = llvm::ImmutableMap<const MemRegion *, bool>;

// Locks currently held on the path, and objects accessed while any tracked
// lock was held (i.e., data protected by a critical section).  ImmutableMap
// traits carry their own factory through get_context; a plain
// REGISTER_TRAIT specialization declares no context_type, so the previous
// set-based state could never be built or consulted.
REGISTER_MAP_WITH_PROGRAMSTATE(HeldLocks, const MemRegion *, bool)
REGISTER_MAP_WITH_PROGRAMSTATE(ProtectedObjs, const MemRegion *, bool)
// Fields already cleared by a tracked report on this path: only the first
// unprotected clear of a given field region is a new unsynchronized access.
REGISTER_MAP_WITH_PROGRAMSTATE(ClearedFields, const MemRegion *, bool)

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

// Membership test over a region->flag map; ImmutableMap iterators expose
// getKey()/getData() pairs.
static bool mapContainsRegion(const RegionMap &M, const MemRegion *R) {
  for (auto It = M.begin(); It != M.end(); ++It) {
    if (It.getKey() == R)
      return true;
  }
  return false;
}

static ProgramStateRef addHeldLock(ProgramStateRef State, const MemRegion *Lock) {
  if (!Lock) return State;
  RegionMap Cur = State->get<HeldLocks>();
  if (mapContainsRegion(Cur, Lock))
    return State;
  RegionMap::Factory &F = State->get_context<HeldLocks>();
  Cur = F.add(Cur, Lock, true);
  return State->set<HeldLocks>(Cur);
}

static ProgramStateRef removeHeldLock(ProgramStateRef State, const MemRegion *Lock) {
  RegionMap Cur = State->get<HeldLocks>();
  if (!Lock) {
    // A release whose lock argument cannot be attributed still ends the
    // critical section for every tracked lock; leaving them held would
    // wrongly silence later unprotected accesses of protected data.
    if (!Cur.isEmpty())
      State = State->set<HeldLocks>(
          State->get_context<HeldLocks>().getEmptyMap());
    return State;
  }
  RegionMap::Factory &F = State->get_context<HeldLocks>();
  Cur = F.remove(Cur, Lock);
  State = State->set<HeldLocks>(Cur);
  return State;
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
  RegionMap Cur = State->get<ProtectedObjs>();
  if (mapContainsRegion(Cur, ObjR))
    return State;
  RegionMap::Factory &F = State->get_context<ProtectedObjs>();
  Cur = F.add(Cur, ObjR, true);
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
  void reportIssue(const BinaryOperator *BO, ProgramStateRef State,
                   CheckerContext &C) const;
};

void SAGenTestChecker::checkBeginFunction(CheckerContext &C) const {
  // Reset per-path lock state only for a top-level function; entering an
  // inlined callee must not drop the caller's held locks.
  if (!C.getStackFrame()->inTopFrame())
    return;

  ProgramStateRef State = C.getState();
  State = State->set<HeldLocks>(State->get_context<HeldLocks>().getEmptyMap());
  State = State->set<ProtectedObjs>(State->get_context<ProtectedObjs>().getEmptyMap());
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

  if (!mapContainsRegion(State->get<ProtectedObjs>(), ObjR))
    return;

  // First-clear semantics: the patch moves this store inside the critical
  // section unconditionally, so the race predicate must not depend on the
  // field's previously bound value.  An unsynchronized write to
  // lock-protected data races with locked readers even when the old binding
  // is still the initial zero (e.g. a zero-initialized caller object whose
  // fields RegionStore binds concretely to NULL).  Only a redundant
  // re-clear of the same field region publishes no new unsynchronized
  // access, so repeats are skipped and the first clear is recorded on the
  // reporting node.
  if (mapContainsRegion(State->get<ClearedFields>(), R))
    return;
  RegionMap::Factory &CF = State->get_context<ClearedFields>();
  State = State->set<ClearedFields>(
      CF.add(State->get<ClearedFields>(), R, true));

  reportIssue(BO, State, C);
}

void SAGenTestChecker::reportIssue(const BinaryOperator *BO, ProgramStateRef State,
                                   CheckerContext &C) const {
  ExplodedNode *N = C.generateNonFatalErrorNode(State);
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
