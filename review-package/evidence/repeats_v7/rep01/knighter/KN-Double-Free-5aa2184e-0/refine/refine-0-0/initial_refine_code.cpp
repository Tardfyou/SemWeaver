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
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"

using namespace clang;
using namespace ento;
using namespace taint;

// true: currently allocated; false: previously freed.
REGISTER_MAP_WITH_PROGRAMSTATE(AllocatedRegionMap, const MemRegion *, bool)

// Maps pointer-storage regions, such as local pointer variables and fields,
// to the region currently stored in them.
REGISTER_MAP_WITH_PROGRAMSTATE(PtrAliasMap, const MemRegion *,
                               const MemRegion *)

namespace {

class SAGenTestChecker
    : public Checker<check::PostCall, check::PreCall, check::Bind> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Incorrect free in error path")) {}

  void checkPostCall(const CallEvent &Call, CheckerContext &C) const;
  void checkPreCall(const CallEvent &Call, CheckerContext &C) const;
  void checkBind(SVal Loc, SVal Val, const Stmt *S,
                 CheckerContext &C) const;

private:
  static bool isAllocationCall(const CallEvent &Call);
  static bool isKfreeCall(const CallEvent &Call);

  const MemRegion *getCanonicalRegion(const MemRegion *MR,
                                      ProgramStateRef State) const;

  // A cleanup helper commonly receives ownership through a formal parameter.
  // A missing allocation record for such a parameter is not enough evidence
  // of an invalid free because the allocation may be in another call context.
  bool isFalsePositive(const CallEvent &Call, ProgramStateRef State,
                       const MemRegion *MR) const;

  void reportIncorrectFree(const CallEvent &Call,
                           CheckerContext &C) const;
};

bool SAGenTestChecker::isAllocationCall(const CallEvent &Call) {
  const IdentifierInfo *ID = Call.getCalleeIdentifier();
  if (!ID)
    return false;

  llvm::StringRef Name = ID->getName();

  // kmemdup() is the allocator used by hws_definer_alloc().  The remaining
  // functions are common kfree-compatible Linux allocation APIs.
  return Name == "kzalloc" ||
         Name == "kmalloc" ||
         Name == "kcalloc" ||
         Name == "kmalloc_array" ||
         Name == "kmemdup" ||
         Name == "kmemdup_nul" ||
         Name == "kstrdup" ||
         Name == "kstrndup";
}

bool SAGenTestChecker::isKfreeCall(const CallEvent &Call) {
  const IdentifierInfo *ID = Call.getCalleeIdentifier();
  return ID && ID->getName() == "kfree";
}

const MemRegion *
SAGenTestChecker::getCanonicalRegion(const MemRegion *MR,
                                     ProgramStateRef State) const {
  if (!MR)
    return nullptr;

  const MemRegion *Current = MR;

  // Alias chains are normally only one level deep, but keep a bounded walk
  // to avoid depending on that implementation detail and to prevent cycles.
  for (unsigned I = 0; I != 8; ++I) {
    const auto *Aliased = State->get<PtrAliasMap>(Current);
    if (!Aliased || !*Aliased || *Aliased == Current)
      break;
    Current = *Aliased;
  }

  return Current->getBaseRegion();
}

bool SAGenTestChecker::isFalsePositive(const CallEvent &Call,
                                       ProgramStateRef State,
                                       const MemRegion *MR) const {
  // If this checker has previously seen the allocation or release, it has
  // enough ownership information to diagnose a release after release.
  if (State->get<AllocatedRegionMap>(MR))
    return false;

  const auto *CE = dyn_cast_or_null<CallExpr>(Call.getOriginExpr());
  if (!CE || CE->getNumArgs() == 0)
    return false;

  const Expr *Arg = CE->getArg(0)->IgnoreParenImpCasts();
  const auto *DRE = dyn_cast<DeclRefExpr>(Arg);
  if (!DRE)
    return false;

  // kfree(definer) in mlx5hws_definer_free() is such a case: definer is a
  // formal parameter, and its allocation belongs to hws_definer_alloc().
  // Suppress only the unproven first free. A known second free still reports.
  return isa<ParmVarDecl>(DRE->getDecl());
}

void SAGenTestChecker::checkPostCall(const CallEvent &Call,
                                     CheckerContext &C) const {
  if (!isAllocationCall(Call))
    return;

  const Expr *OriginExpr = Call.getOriginExpr();
  const auto *CE = dyn_cast_or_null<CallExpr>(OriginExpr);
  if (!CE)
    return;

  const MemRegion *MR = getMemRegionFromExpr(CE, C);
  if (!MR)
    return;

  MR = MR->getBaseRegion();
  if (!MR)
    return;

  ProgramStateRef State = C.getState();

  // A new allocation re-establishes ownership even if an earlier, unrelated
  // allocation was previously released.
  State = State->set<AllocatedRegionMap>(MR, true);
  C.addTransition(State);
}

void SAGenTestChecker::checkPreCall(const CallEvent &Call,
                                    CheckerContext &C) const {
  if (!isKfreeCall(Call))
    return;

  if (Call.getNumArgs() < 1)
    return;

  const MemRegion *MR = Call.getArgSVal(0).getAsRegion();
  if (!MR)
    return; // kfree(NULL) and non-region values do not need a diagnostic.

  ProgramStateRef State = C.getState();
  MR = getCanonicalRegion(MR, State);
  if (!MR)
    return;

  const bool *IsAllocated = State->get<AllocatedRegionMap>(MR);

  if (!IsAllocated || !*IsAllocated) {
    // A released region is a proven double free and must always be reported.
    // An untracked field such as mt->fc is intentionally still reported:
    // this preserves detection of the erroneous free_fc cleanup path.
    if (!isFalsePositive(Call, State, MR))
      reportIncorrectFree(Call, C);
    return;
  }

  // Preserve an explicit "released" state. Removing the entry would make a
  // second free indistinguishable from an allocation unseen in this context.
  State = State->set<AllocatedRegionMap>(MR, false);
  C.addTransition(State);
}

void SAGenTestChecker::checkBind(SVal Loc, SVal Val, const Stmt *S,
                                 CheckerContext &C) const {
  const MemRegion *Storage = Loc.getAsRegion();
  if (!Storage)
    return;

  ProgramStateRef State = C.getState();
  ProgramStateRef NewState = State;

  if (const MemRegion *Pointee = Val.getAsRegion()) {
    Pointee = getCanonicalRegion(Pointee, State);
    if (Pointee)
      NewState = NewState->set<PtrAliasMap>(Storage, Pointee);
  } else {
    // A null/non-pointer assignment invalidates any old alias associated with
    // this pointer storage location.
    NewState = NewState->remove<PtrAliasMap>(Storage);
  }

  if (NewState != State)
    C.addTransition(NewState);
}

void SAGenTestChecker::reportIncorrectFree(const CallEvent &Call,
                                           CheckerContext &C) const {
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT,
      "Incorrect free in error handling: pointer is being freed without a "
      "valid allocation",
      N);
  Report->addRange(Call.getSourceRange());
  C.emitReport(std::move(Report));
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects cleanup paths that free unallocated resources, which may "
      "indicate a double free error",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
