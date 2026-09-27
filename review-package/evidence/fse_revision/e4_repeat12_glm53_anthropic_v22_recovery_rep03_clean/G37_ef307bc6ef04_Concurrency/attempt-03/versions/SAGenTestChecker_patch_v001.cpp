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
#include "clang/AST/ASTContext.h"
#include "clang/AST/ParentMapContext.h"
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
REGISTER_TRAIT_WITH_PROGRAMSTATE(LastUnlockedLock, const MemRegion*)
REGISTER_TRAIT_WITH_PROGRAMSTATE(ObjSetTrait, ObjSet)

//====================== Helpers ============================

static bool isOneOf(StringRef S, std::initializer_list<StringRef> Names) {
  for (auto &N : Names)
    if (S == N) return true;
  return false;
}

// Kernel builds may resolve spin_lock_irqsave()/spin_unlock_irqrestore()
// through optional raw_/_raw_/__raw_ wrapper layers; the lock primitive
// keeps its exact spelling after stripping those prefixes.
static StringRef stripSpinWrapperPrefix(StringRef Name) {
  for (StringRef P : {"__raw_", "_raw_", "raw_"}) {
    if (Name.startswith(P))
      return Name.drop_front(P.size());
  }
  return Name;
}

static bool isLockAcquire(const CallEvent &Call, CheckerContext &C) {
  if (const IdentifierInfo *ID = Call.getCalleeIdentifier()) {
    StringRef Name = stripSpinWrapperPrefix(ID->getName());
    return isOneOf(Name, {"spin_lock", "spin_lock_irqsave", "spin_lock_bh", "spin_lock_irq"});
  }
  // Fallback to textual check
  const Expr *E = Call.getOriginExpr();
  if (!E) return false;
  return ExprHasName(E, "spin_lock_irqsave", C) ||
         ExprHasName(E, "spin_lock_irq", C) ||
         ExprHasName(E, "spin_lock_bh", C) ||
         ExprHasName(E, "spin_lock", C);
}

static bool isLockRelease(const CallEvent &Call, CheckerContext &C) {
  if (const IdentifierInfo *ID = Call.getCalleeIdentifier()) {
    StringRef Name = stripSpinWrapperPrefix(ID->getName());
    return isOneOf(Name, {"spin_unlock", "spin_unlock_irqrestore", "spin_unlock_bh", "spin_unlock_irq"});
  }
  const Expr *E = Call.getOriginExpr();
  if (!E) return false;
  return ExprHasName(E, "spin_unlock_irqrestore", C) ||
         ExprHasName(E, "spin_unlock_irq", C) ||
         ExprHasName(E, "spin_unlock_bh", C) ||
         ExprHasName(E, "spin_unlock", C);
}

static const MemRegion *getLockRegionFromCall(const CallEvent &Call, CheckerContext &C) {
  if (Call.getNumArgs() < 1)
    return nullptr;
  // Prefer the evaluated argument value: this resolves `lock` and `&obj->lock`
  // argument forms uniformly, independent of syntactic wrappers.
  if (const MemRegion *R = Call.getArgSVal(0).getAsRegion())
    return R->getBaseRegion();
  const Expr *Arg0 = Call.getArgExpr(0);
  if (!Arg0)
    return nullptr;
  const MemRegion *R = getMemRegionFromExpr(Arg0, C);
  if (!R)
    return nullptr;
  return R->getBaseRegion();
}

static ProgramStateRef addHeldLock(ProgramStateRef State, const MemRegion *Lock, CheckerContext &C) {
  if (!Lock) return State;
  LockSet Cur = State->get<HeldLocks>();
  LockSet::Factory &F = State->get_context<HeldLocks>();
  Cur = F.add(Cur, Lock);
  State = State->set<HeldLocks>(Cur);
  // When we start holding a lock again, the "just unlocked" window is over.
  State = State->set<LastUnlockedLock>(nullptr);
  return State;
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

// Return the MemberExpr whose member is accessed by S: either S itself
// (after stripping parentheses/implicit casts) or its immediate AST parent.
static const MemberExpr *getMemberExprForAccess(const Stmt *S,
                                                CheckerContext &C) {
  if (!S)
    return nullptr;
  if (const Expr *E = dyn_cast<Expr>(S))
    if (const auto *ME = dyn_cast<MemberExpr>(E->IgnoreParenImpCasts()))
      return ME;
  const auto &Parents = C.getASTContext().getParents(*S);
  for (const auto &P : Parents)
    if (const auto *ME = P.get<MemberExpr>())
      return ME;
  return nullptr;
}

// Record the region "R" (and its base object) as state protected by each
// lock currently held in HeldLocks.
static ProgramStateRef recordRegionUnderHeldLocks(ProgramStateRef State,
                                                  const MemRegion *R) {
  if (!R)
    return State;
  LockSet Locks = State->get<HeldLocks>();
  if (Locks.isEmpty())
    return State;

  const MemRegion *BaseR = R->getBaseRegion();
  ObjSet::Factory &OF = State->get_context<ObjSetTrait>();
  for (auto LI = Locks.begin(); LI != Locks.end(); ++LI) {
    const MemRegion *LockR = *LI;
    if (!LockR) continue;
    const ObjSet *CurPtr = State->get<LockToObjSetMap>(LockR);
    ObjSet Cur = CurPtr ? *CurPtr : OF.getEmptySet();
    bool Changed = false;
    if (!objSetContains(Cur, R)) {
      Cur = OF.add(Cur, R);
      Changed = true;
    }
    if (BaseR && BaseR != R && !objSetContains(Cur, BaseR)) {
      Cur = OF.add(Cur, BaseR);
      Changed = true;
    }
    if (Changed)
      State = State->set<LockToObjSetMap>(LockR, Cur);
  }
  return State;
}

// Expression-based fallback for arguments whose evaluated value carries no
// region; still binds the accessed region to the currently held locks.
static ProgramStateRef recordObjUseUnderHeldLocks(ProgramStateRef State,
                                                  const Expr *ArgE,
                                                  CheckerContext &C) {
  if (!ArgE)
    return State;
  if (State->get<HeldLocks>().isEmpty())
    return State;

  const MemRegion *ObjR = nullptr;

  // If it's a member access, prefer to use the base object's region.
  if (const auto *ME = dyn_cast<MemberExpr>(ArgE->IgnoreParenCasts())) {
    const Expr *BaseE = ME->getBase();
    ObjR = C.getSVal(BaseE).getAsRegion();
    if (!ObjR)
      ObjR = getMemRegionFromExpr(BaseE, C);
  } else {
    ObjR = C.getSVal(ArgE).getAsRegion();
    if (!ObjR)
      ObjR = getMemRegionFromExpr(ArgE, C);
  }

  if (!ObjR)
    return State;
  return recordRegionUnderHeldLocks(State, ObjR);
}

static bool isZeroSVal(SVal V) {
  if (auto CI = V.getAs<nonloc::ConcreteInt>())
    return CI->getValue() == 0;
  if (auto LCI = V.getAs<loc::ConcreteInt>())
    return LCI->getValue() == 0;
  return false;
}

static bool isPointerFieldAssignment(const Stmt *S, SVal Loc, SVal Val,
                                     CheckerContext &C,
                                     const BinaryOperator *&OutBO,
                                     const MemberExpr *&OutME) {
  OutBO = nullptr;
  OutME = nullptr;

  const auto *BO = dyn_cast_or_null<BinaryOperator>(S);
  if (!BO) {
    // The store statement may be the member expression of an assignment's
    // LHS; recover the assignment through the AST parents.
    if (S) {
      for (const auto &P : C.getASTContext().getParents(*S))
        if (const auto *PBO = P.get<BinaryOperator>())
          if (PBO->isAssignmentOp()) {
            BO = PBO;
            break;
          }
    }
    if (!BO)
      return false;
  }
  if (!BO->isAssignmentOp())
    return false;

  const Expr *LHS = BO->getLHS()->IgnoreParenCasts();
  const auto *ME = dyn_cast<MemberExpr>(LHS);
  if (!ME)
    return false;

  // We focus on pointer field like obj->field.
  if (!ME->isArrow())
    return false;

  QualType FieldTy = ME->getType();
  if (FieldTy.isNull() || !FieldTy->isPointerType())
    return false;

  OutBO = BO;
  OutME = ME;
  return true;
}

static bool isPointerFieldAssignmentToNull(const Stmt *S, SVal Loc, SVal Val,
                                           CheckerContext &C,
                                           const BinaryOperator *&OutBO,
                                           const MemberExpr *&OutME) {
  if (!isPointerFieldAssignment(S, Loc, Val, C, OutBO, OutME))
    return false;

  bool IsNull = isZeroSVal(Val);
  if (!IsNull) {
    // Try evaluate RHS constant int == 0
    llvm::APSInt IntV;
    if (EvaluateExprToInt(IntV, OutBO->getRHS(), C))
      IsNull = (IntV == 0);
  }
  return IsNull;
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
  // Inlined callees must not reset lock state tracked for the caller frame,
  // otherwise an inlined call inside the critical section would drop HeldLocks.
  if (!C.inTopFrame())
    return;

  ProgramStateRef State = C.getState();

  // Initialize/clear per-function state.
  State = State->set<HeldLocks>(State->get_context<HeldLocks>().getEmptySet());
  State = State->set<LastUnlockedLock>(nullptr);
  // Per-function handoff witnesses must not leak across analyzed roots.
  State = State->set<ObjSetTrait>(State->get_context<ObjSetTrait>().getEmptySet());

  C.addTransition(State);
}

void SAGenTestChecker::checkPreCall(const CallEvent &Call, CheckerContext &C) const {
  ProgramStateRef State = C.getState();

  // Handle lock acquire
  if (isLockAcquire(Call, C)) {
    if (const MemRegion *LockR = getLockRegionFromCall(Call, C)) {
      State = addHeldLock(State, LockR, C);
    }
    C.addTransition(State);
    return;
  }

  // While locks are held, record every region/pointer argument flowing
  // through the critical section as state protected by the held locks.
  if (!State->get<HeldLocks>().isEmpty()) {
    ProgramStateRef NewState = State;
    for (unsigned i = 0; i < Call.getNumArgs(); ++i) {
      if (const MemRegion *ArgR = Call.getArgSVal(i).getAsRegion())
        NewState = recordRegionUnderHeldLocks(NewState, ArgR);
      else
        NewState = recordObjUseUnderHeldLocks(NewState, Call.getArgExpr(i), C);
    }
    if (NewState != State)
      C.addTransition(NewState);
    return;
  }

  // No lock is held, but one was just released on this path: any
  // intervening call may itself take that lock or complete the teardown
  // of the protected object, so the escape window opened by the release
  // ends here. A NULL store after such a call no longer escapes the
  // critical section through this window.
  if (State->get<LastUnlockedLock>()) {
    C.addTransition(State->set<LastUnlockedLock>(nullptr));
  }
}

void SAGenTestChecker::checkPostCall(const CallEvent &Call, CheckerContext &C) const {
  ProgramStateRef State = C.getState();

  // Handle lock release
  if (isLockRelease(Call, C)) {
    const MemRegion *LockR = getLockRegionFromCall(Call, C);
    State = removeHeldLock(State, LockR);
    // Remember the released lock: stores to its protected state now escape
    // the critical section until the lock is acquired again.
    State = State->set<LastUnlockedLock>(LockR);
    C.addTransition(State);
    return;
  }
}

void SAGenTestChecker::checkLocation(SVal Loc, bool IsLoad, const Stmt *S, CheckerContext &C) const {
  ProgramStateRef State = C.getState();
  if (State->get<HeldLocks>().isEmpty())
    return;

  ProgramStateRef NewState = State;
  if (const MemRegion *R = Loc.getAsRegion()) {
    // Any field/object location touched while the lock is held is protected
    // state of that lock; record it at region granularity.
    NewState = recordRegionUnderHeldLocks(State, R);
  } else {
    // Fallback: recover the accessed member expression from the AST.
    const MemberExpr *ME = getMemberExprForAccess(S, C);
    if (ME && ME->getBase())
      NewState = recordObjUseUnderHeldLocks(State, ME->getBase(), C);
  }
  if (NewState != State)
    C.addTransition(NewState);
}

void SAGenTestChecker::checkBind(SVal Loc, SVal Val, const Stmt *S, CheckerContext &C) const {
  ProgramStateRef State = C.getState();

  // A store executed while locks are held updates state protected by those locks.
  if (!State->get<HeldLocks>().isEmpty()) {
    if (const MemRegion *R = Loc.getAsRegion()) {
      ProgramStateRef NewState = recordRegionUnderHeldLocks(State, R);
      if (NewState != State)
        C.addTransition(NewState);
    }
    return;
  }

  const MemRegion *JustUnlocked = State->get<LastUnlockedLock>();
  // With no lock currently held and a recently released lock, a NULL store
  // to a pointer field of that lock's protected state escapes the critical
  // section (races with readers that take the lock).
  if (JustUnlocked) {
    const BinaryOperator *BO = nullptr;
    const MemberExpr *ME = nullptr;

    if (isPointerFieldAssignmentToNull(S, Loc, Val, C, BO, ME)) {
      // Prefer the exact bound location region; fall back to the LHS base.
      const MemRegion *StoredR = Loc.getAsRegion();
      const MemRegion *ObjR = StoredR ? StoredR->getBaseRegion()
                                      : getBaseObjectRegionFromLHS(BO, C);
      if (ObjR) {
        // The write races if this object (or the exact field region) was
        // protected state of the released lock on this path, or if this
        // function earlier handed pointer state of the same base object
        // off before the critical section (obj->ptr = fresh); clearing
        // that pointer state after the release escapes the same lock
        // protection the handoff entered.
        const ObjSet *UsedObjsPtr = State->get<LockToObjSetMap>(JustUnlocked);
        ObjSet Handoff = State->get<ObjSetTrait>();
        if ((UsedObjsPtr &&
             (objSetContains(*UsedObjsPtr, ObjR) ||
              (StoredR && objSetContains(*UsedObjsPtr, StoredR)))) ||
            objSetContains(Handoff, ObjR)) {
          reportIssue(BO, C);
        }
      }
    }
    return;
  }

  // Outside all lock coverage (no lock held and no open release window),
  // remember pointer-field handoff stores such as obj->ptr = fresh: they
  // witness that this function set up pointer state of that base object
  // which must later only be cleared while the guarding lock is held.
  const BinaryOperator *HBO = nullptr;
  const MemberExpr *HME = nullptr;
  if (isPointerFieldAssignment(S, Loc, Val, C, HBO, HME)) {
    const MemRegion *StoredR = Loc.getAsRegion();
    const MemRegion *ObjR = StoredR ? StoredR->getBaseRegion()
                                    : getBaseObjectRegionFromLHS(HBO, C);
    if (ObjR) {
      ObjSet Handoff = State->get<ObjSetTrait>();
      ObjSet::Factory &OF = State->get_context<ObjSetTrait>();
      bool Changed = false;
      if (!objSetContains(Handoff, ObjR)) {
        Handoff = OF.add(Handoff, ObjR);
        Changed = true;
      }
      if (StoredR && !objSetContains(Handoff, StoredR)) {
        Handoff = OF.add(Handoff, StoredR);
        Changed = true;
      }
      if (Changed)
        C.addTransition(State->set<ObjSetTrait>(Handoff));
    }
  }
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
