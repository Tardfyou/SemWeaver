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
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
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

REGISTER_TRAIT_WITH_PROGRAMSTATE(HeldLocks, LockSet)
REGISTER_MAP_WITH_PROGRAMSTATE(LockToObjSetMap, const MemRegion*, ObjSet)
REGISTER_TRAIT_WITH_PROGRAMSTATE(ObjSetTrait, ObjSet)

// Inverse protection view: object -> locks under which this object (or its
// fields) was observed in this function. A NULL teardown of a pointer field
// of such an object while none of these locks is held runs outside the
// critical sections that use the field.
REGISTER_MAP_WITH_PROGRAMSTATE(ObjProtectLocks, const MemRegion*, LockSet)

// Pointer fields of an object published (written) by this function on the
// same path, kept to relate teardowns and publications of protected fields.
using FieldSet = llvm::ImmutableSet<const FieldDecl *>;

REGISTER_MAP_WITH_PROGRAMSTATE(FieldWrites, const MemRegion *, FieldSet)

// Registration by value: objects whose address was stored into a pointer
// field of another object (e.g. dwc2_urb->priv = urb). Lock protection is
// propagated from an object to the objects registered into it, so the
// teardown of a registered object's field is tied to the same lock relation.
REGISTER_MAP_WITH_PROGRAMSTATE(ValueRegistered, const MemRegion *, ObjSet)

// The lock released most recently whose unlock window has not yet been
// consumed by another call; relates a teardown write to the just-ended
// critical section in program state.
REGISTER_TRAIT_WITH_PROGRAMSTATE(LastUnlockedLock, const MemRegion *)

//====================== Helpers ============================

// The kernel spin-lock macros preprocess into raw primitives (e.g.
// _raw_spin_lock_irqsave / _raw_spin_unlock_irqrestore), so lock-state
// recognition must match the preprocessed callee names, not only the
// source-level wrapper spellings. These predicates only feed the lock-state
// machine below (held-lock set, objects used under a lock, just-unlocked
// window); the warning predicate remains that state relation.
static bool isSpinLockAcquireName(StringRef Name) {
  return Name.contains("spin_lock");
}

static bool isSpinLockReleaseName(StringRef Name) {
  return Name.contains("spin_unlock");
}

static bool isLockAcquire(const CallEvent &Call, CheckerContext &C) {
  if (const IdentifierInfo *ID = Call.getCalleeIdentifier())
    return isSpinLockAcquireName(ID->getName());
  const Expr *E = Call.getOriginExpr();
  if (!E) return false;
  return ExprHasName(E, "spin_lock", C);
}

static bool isLockRelease(const CallEvent &Call, CheckerContext &C) {
  if (const IdentifierInfo *ID = Call.getCalleeIdentifier())
    return isSpinLockReleaseName(ID->getName());
  const Expr *E = Call.getOriginExpr();
  if (!E) return false;
  return ExprHasName(E, "spin_unlock", C);
}

static const MemRegion *getLockRegionFromCall(const CallEvent &Call, CheckerContext &C) {
  if (Call.getNumArgs() < 1)
    return nullptr;
  const Expr *Arg0 = Call.getArgExpr(0);
  if (!Arg0)
    return nullptr;
  const MemRegion *R = getMemRegionFromExpr(Arg0, C);
  if (!R)
    return nullptr;
  return R->getBaseRegion();
}

// Call events are not delivered for inlined callees, so the lock-state machine
// must also recognize the same primitives directly on CallExpr statements;
// both paths drive the identical idempotent state updates.
static bool callExprHasName(const CallExpr *CE, StringRef Sub, CheckerContext &C) {
  if (const FunctionDecl *FD = CE->getDirectCallee())
    if (FD->getName().contains(Sub))
      return true;
  const Expr *CalleeE = CE->getCallee();
  return CalleeE && ExprHasName(CalleeE, Sub, C);
}

static bool isAcquireCallExpr(const CallExpr *CE, CheckerContext &C) {
  return callExprHasName(CE, "spin_lock", C);
}

static bool isReleaseCallExpr(const CallExpr *CE, CheckerContext &C) {
  return callExprHasName(CE, "spin_unlock", C);
}

static const MemRegion *getLockRegionFromCallExpr(const CallExpr *CE, CheckerContext &C) {
  if (CE->getNumArgs() < 1)
    return nullptr;
  const MemRegion *R = getMemRegionFromExpr(CE->getArg(0), C);
  if (!R)
    return nullptr;
  return R->getBaseRegion();
}

static bool lockSetContains(const LockSet &S, const MemRegion *R);

// True when any lock that protects the object is currently held; a teardown
// whose protecting locks are all released runs outside every critical section
// that can observe its fields.
static bool anyProtectLockHeld(ProgramStateRef State, const LockSet &Prot) {
  LockSet Held = State->get<HeldLocks>();
  for (auto LI = Prot.begin(); LI != Prot.end(); ++LI) {
    const MemRegion *L = *LI;
    if (L && lockSetContains(Held, L))
      return true;
  }
  return false;
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

static bool fieldSetContains(const FieldSet &S, const FieldDecl *FD) {
  for (auto It = S.begin(); It != S.end(); ++It) {
    if (*It == FD)
      return true;
  }
  return false;
}

static bool lockSetContains(const LockSet &S, const MemRegion *R) {
  for (auto It = S.begin(); It != S.end(); ++It) {
    if (*It == R)
      return true;
  }
  return false;
}

// Bind an object to a lock under which it was observed; the teardown check
// later requires every protecting lock of the written object to be held.
static ProgramStateRef addObjProtection(ProgramStateRef State, const MemRegion *ObjR,
                                        const MemRegion *LockR) {
  if (!ObjR || !LockR) return State;
  LockSet::Factory &LF = State->get_context<HeldLocks>();
  const LockSet *ProtPtr = State->get<ObjProtectLocks>(ObjR);
  LockSet Prot = ProtPtr ? *ProtPtr : LF.getEmptySet();
  if (lockSetContains(Prot, LockR))
    return State;
  Prot = LF.add(Prot, LockR);
  return State->set<ObjProtectLocks>(ObjR, Prot);
}

// Record that "ObjR" is used while holding each lock in HeldLocks.
static ProgramStateRef recordObjUseUnderHeldLocks(ProgramStateRef State,
                                                  const Expr *ArgE,
                                                  CheckerContext &C) {
  if (!ArgE)
    return State;
  LockSet Locks = State->get<HeldLocks>();
  if (Locks.isEmpty())
    return State;

  const MemRegion *ObjR = nullptr;

  // If it's a member access, prefer to use the base object's region.
  if (const auto *ME = dyn_cast<MemberExpr>(ArgE->IgnoreParenCasts())) {
    const Expr *BaseE = ME->getBase();
    ObjR = getMemRegionFromExpr(BaseE, C);
  } else {
    ObjR = getMemRegionFromExpr(ArgE, C);
  }

  if (!ObjR)
    return State;
  ObjR = ObjR->getBaseRegion();
  if (!ObjR)
    return State;

  ObjSet::Factory &OF = State->get_context<ObjSetTrait>();
  for (auto LI = Locks.begin(); LI != Locks.end(); ++LI) {
    const MemRegion *LockR = *LI;
    if (!LockR) continue;
    State = addObjProtection(State, ObjR, LockR);
    const ObjSet *CurPtr = State->get<LockToObjSetMap>(LockR);
    ObjSet Cur = CurPtr ? *CurPtr : OF.getEmptySet();
    if (!objSetContains(Cur, ObjR)) {
      Cur = OF.add(Cur, ObjR);
      State = State->set<LockToObjSetMap>(LockR, Cur);
    }
    // Lock protection covers objects registered by value into any member of
    // the protected set: their fields belong to the same lock relation.
    bool Grew = true;
    while (Grew) {
      Grew = false;
      auto RegMap = State->get<ValueRegistered>();
      for (auto RI = RegMap.begin(); RI != RegMap.end(); ++RI) {
        const MemRegion *RegisteredR = RI->first;
        if (!RegisteredR || objSetContains(Cur, RegisteredR))
          continue;
        bool Linked = false;
        for (auto UI = RI->second.begin(); UI != RI->second.end(); ++UI) {
          if (*UI && objSetContains(Cur, *UI)) {
            Linked = true;
            break;
          }
        }
        if (Linked) {
          Cur = OF.add(Cur, RegisteredR);
          State = State->set<LockToObjSetMap>(LockR, Cur);
          State = addObjProtection(State, RegisteredR, LockR);
          Grew = true;
        }
      }
    }
  }
  return State;
}

static bool isZeroSVal(SVal V) {
  if (auto CI = V.getAs<nonloc::ConcreteInt>())
    return CI->getValue() == 0;
  if (auto LCI = V.getAs<loc::ConcreteInt>())
    return LCI->getValue() == 0;
  return false;
}

// Matches statements of the form obj->ptr_field = ..., which model both the
// publication and the teardown of lock-protected pointer fields.
static bool getArrowPointerFieldAssign(const Stmt *S, const BinaryOperator *&OutBO,
                                       const MemberExpr *&OutME) {
  OutBO = nullptr;
  OutME = nullptr;

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

  OutBO = BO;
  OutME = ME;
  return true;
}

static bool isStoreOfNull(SVal Val, const Expr *RHS, CheckerContext &C) {
  if (isZeroSVal(Val))
    return true;
  llvm::APSInt IntV;
  return RHS && EvaluateExprToInt(IntV, RHS, C) && (IntV == 0);
}

static const MemRegion *getBaseObjectRegionFromLHS(const BinaryOperator *BO,
                                                   CheckerContext &C) {
  if (!BO) return nullptr;
  const auto *ME = dyn_cast<MemberExpr>(BO->getLHS()->IgnoreParenCasts());
  if (!ME) return nullptr;
  const Expr *BaseE = ME->getBase();
  if (!BaseE) return nullptr;
  const MemRegion *R = getMemRegionFromExpr(BaseE, C);
  if (!R) return nullptr;
  return R->getBaseRegion();
}

//====================== Checker ============================

namespace {

class SAGenTestChecker
  : public Checker<check::BeginFunction,
                   check::PreCall,
                   check::PostCall,
                   check::PreStmt<CallExpr>,
                   check::PostStmt<CallExpr>,
                   check::Location,
                   check::Bind> {
  mutable std::unique_ptr<BugType> BT;
  // Factory for the per-path sets of published pointer fields.
  mutable FieldSet::Factory FieldSetF;

public:
  SAGenTestChecker() : BT(new BugType(this, "Pointer field NULLed after unlock", "Concurrency")) {}

  void checkBeginFunction(CheckerContext &C) const;
  void checkPreCall(const CallEvent &Call, CheckerContext &C) const;
  void checkPostCall(const CallEvent &Call, CheckerContext &C) const;
  void checkPreStmt(const CallExpr *CE, CheckerContext &C) const;
  void checkPostStmt(const CallExpr *CE, CheckerContext &C) const;
  void checkLocation(SVal Loc, bool IsLoad, const Stmt *S, CheckerContext &C) const;
  void checkBind(SVal Loc, SVal Val, const Stmt *S, CheckerContext &C) const;

private:
  void reportIssue(const BinaryOperator *BO, CheckerContext &C) const;
};

void SAGenTestChecker::checkBeginFunction(CheckerContext &C) const {
  ProgramStateRef State = C.getState();

  // Initialize/clear per-function state.
  State = State->set<HeldLocks>(State->get_context<HeldLocks>().getEmptySet());

  C.addTransition(State);
}

void SAGenTestChecker::checkPreCall(const CallEvent &Call, CheckerContext &C) const {
  ProgramStateRef State = C.getState();

  // Handle lock acquire
  if (isLockAcquire(Call, C)) {
    if (const MemRegion *LockR = getLockRegionFromCall(Call, C)) {
      State = addHeldLock(State, LockR);
    }
    C.addTransition(State);
    return;
  }

  // While locks are held, record any object/pointer arguments used inside the critical section.
  if (!State->get<HeldLocks>().isEmpty()) {
    for (unsigned i = 0; i < Call.getNumArgs(); ++i) {
      const Expr *ArgE = Call.getArgExpr(i);
      State = recordObjUseUnderHeldLocks(State, ArgE, C);
    }
    C.addTransition(State);
  }
}

void SAGenTestChecker::checkPostCall(const CallEvent &Call, CheckerContext &C) const {
  ProgramStateRef State = C.getState();

  // Handle lock release
  if (isLockRelease(Call, C)) {
    const MemRegion *LockR = getLockRegionFromCall(Call, C);
    State = removeHeldLock(State, LockR);
    // Mark the last unlocked lock to detect the immediate next assignment.
    State = State->set<LastUnlockedLock>(LockR);
    C.addTransition(State);
    return;
  }

  // Any other call after unlock clears the "just unlocked" mark to keep window small.
  if (State->get<LastUnlockedLock>()) {
    State = State->set<LastUnlockedLock>(nullptr);
    C.addTransition(State);
  }
}

void SAGenTestChecker::checkPreStmt(const CallExpr *CE, CheckerContext &C) const {
  ProgramStateRef State = C.getState();

  // Mirror of the call-event acquire handling: the lock-state machine must
  // also engage when the spinlock primitive is inlined and no call event is
  // delivered; the update is idempotent with checkPreCall.
  if (isAcquireCallExpr(CE, C)) {
    if (const MemRegion *LockR = getLockRegionFromCallExpr(CE, C))
      State = addHeldLock(State, LockR);
    C.addTransition(State);
  }
}

void SAGenTestChecker::checkPostStmt(const CallExpr *CE, CheckerContext &C) const {
  ProgramStateRef State = C.getState();

  // Mirror of the call-event release handling (idempotent with checkPostCall).
  if (isReleaseCallExpr(CE, C)) {
    const MemRegion *LockR = getLockRegionFromCallExpr(CE, C);
    State = removeHeldLock(State, LockR);
    State = State->set<LastUnlockedLock>(LockR);
    C.addTransition(State);
    return;
  }

  // Keep the unlock-window maintenance identical on the statement path.
  if (State->get<LastUnlockedLock>()) {
    State = State->set<LastUnlockedLock>(nullptr);
    C.addTransition(State);
  }
}

void SAGenTestChecker::checkLocation(SVal Loc, bool IsLoad, const Stmt *S, CheckerContext &C) const {
  ProgramStateRef State = C.getState();
  if (State->get<HeldLocks>().isEmpty())
    return;

  // When under a lock, if the location is part of a MemberExpr, record the base object.
  const MemberExpr *ME = findSpecificTypeInParents<MemberExpr>(S, C);
  if (!ME)
    return;

  const Expr *BaseE = ME->getBase();
  if (!BaseE)
    return;

  State = recordObjUseUnderHeldLocks(State, BaseE, C);
  C.addTransition(State);
}

void SAGenTestChecker::checkBind(SVal Loc, SVal Val, const Stmt *S, CheckerContext &C) const {
  ProgramStateRef State = C.getState();

  const BinaryOperator *BO = nullptr;
  const MemberExpr *ME = nullptr;
  if (!getArrowPointerFieldAssign(S, BO, ME))
    return;

  // The object whose pointer field is written, and the field itself.
  const MemRegion *ObjR = getBaseObjectRegionFromLHS(BO, C);
  const FieldDecl *FD = dyn_cast_or_null<FieldDecl>(ME->getMemberDecl());
  if (!ObjR || !FD)
    return;

  // A field written inside a critical section stays protected by that lock
  // for the rest of the path; tie the protection to the write itself so it
  // does not depend on statement adjacency or on later call events.
  bool Changed = false;
  if (!State->get<HeldLocks>().isEmpty()) {
    ProgramStateRef Before = State;
    State = recordObjUseUnderHeldLocks(State, ME->getBase(), C);
    Changed = (State != Before);
  }

  const FieldSet *Written = State->get<FieldWrites>(ObjR);
  const MemRegion *JustUnlocked = State->get<LastUnlockedLock>();
  if (State->get<HeldLocks>().isEmpty() && isStoreOfNull(Val, BO->getRHS(), C)) {
    bool PublishedEarlier = Written && fieldSetContains(*Written, FD);
    if (JustUnlocked) {
      // Teardown right after the release of the lock whose critical section
      // uses this object: the post-unlock teardown window.
      const ObjSet *UsedObjsPtr = State->get<LockToObjSetMap>(JustUnlocked);
      bool UsedUnderLock = UsedObjsPtr && objSetContains(*UsedObjsPtr, ObjR);
      if (PublishedEarlier || UsedUnderLock)
        reportIssue(BO, C);
    } else if (PublishedEarlier) {
      // Durable relation: the same field was published while a lock was held
      // earlier on this path and is now NULLed while none of its protecting
      // locks is held, i.e. outside every critical section that can observe
      // the field. Calls between the unlock and the teardown do not remove
      // this relation.
      const LockSet *Prot = State->get<ObjProtectLocks>(ObjR);
      if (Prot && !Prot->isEmpty() && !anyProtectLockHeld(State, *Prot))
        reportIssue(BO, C);
    }
  }

  // Remember this publication of the pointer field for later teardown checks.
  if (!Written || !fieldSetContains(*Written, FD)) {
    FieldSet Cur = Written ? *Written : FieldSetF.getEmptySet();
    Cur = FieldSetF.add(Cur, FD);
    State = State->set<FieldWrites>(ObjR, Cur);
    Changed = true;
  }

  // Record registration by value: storing a pointer to some object into a
  // pointer field of ObjR places that object under ObjR's ownership, so lock
  // protection later propagates to it (e.g. dwc2_urb->priv = urb).
  if (const Expr *RHSE = BO->getRHS()) {
    const MemRegion *ValR = getMemRegionFromExpr(RHSE->IgnoreParenCasts(), C);
    if (ValR) {
      ValR = ValR->getBaseRegion();
      if (ValR && ValR != ObjR) {
        const ObjSet *RegPtr = State->get<ValueRegistered>(ValR);
        ObjSet Reg = RegPtr ? *RegPtr : State->get_context<ObjSetTrait>().getEmptySet();
        if (!objSetContains(Reg, ObjR)) {
          ObjSet::Factory &OF = State->get_context<ObjSetTrait>();
          Reg = OF.add(Reg, ObjR);
          State = State->set<ValueRegistered>(ValR, Reg);
          Changed = true;
        }
      }
    }
  }

  if (Changed)
    C.addTransition(State);
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
