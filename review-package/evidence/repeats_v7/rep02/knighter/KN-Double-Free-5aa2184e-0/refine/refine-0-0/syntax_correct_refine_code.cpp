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

// A region is present while the checker knows it has been allocated and has
// not yet been released on the current analyzer path.
REGISTER_MAP_WITH_PROGRAMSTATE(AllocatedRegionMap, const MemRegion *, bool)

// A region is present after a modeled deallocation. This is the evidence
// required before reporting a second release.
REGISTER_MAP_WITH_PROGRAMSTATE(ReleasedRegionMap, const MemRegion *, bool)

// Maps pointer storage (for example, a local pointer variable or a field) to
// the region currently stored in it. Storage regions deliberately remain
// uncanonicalized so distinct fields do not collapse to the same base region.
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
  static bool hasCalleeName(const CallEvent &Call, llvm::StringRef Name);
  static bool isAllocationCall(const CallEvent &Call);
  static bool isDeallocationCall(const CallEvent &Call);

  static const MemRegion *getBaseRegion(SVal Value);
  static const MemRegion *getTrackedRegion(SVal Value,
                                           ProgramStateRef State);

  // An untracked pointer may be a formal parameter, global ownership, a
  // custom allocator result, or a deliberate kfree(NULL). It is not enough
  // evidence to diagnose an invalid free.
  static bool isFalsePositive(ProgramStateRef State,
                              const MemRegion *Region);

  void reportIncorrectFree(const CallEvent &Call, CheckerContext &C) const;
};

bool SAGenTestChecker::hasCalleeName(const CallEvent &Call,
                                     llvm::StringRef Name) {
  const IdentifierInfo *II = Call.getCalleeIdentifier();
  return II && II->getName() == Name;
}

bool SAGenTestChecker::isAllocationCall(const CallEvent &Call) {
  // Keep this list intentionally explicit. Matching arbitrary names containing
  // "alloc" would incorrectly treat borrowed-pointer APIs as allocators.
  return hasCalleeName(Call, "malloc") ||
         hasCalleeName(Call, "calloc") ||
         hasCalleeName(Call, "kmalloc") ||
         hasCalleeName(Call, "__kmalloc") ||
         hasCalleeName(Call, "kzalloc") ||
         hasCalleeName(Call, "kcalloc") ||
         hasCalleeName(Call, "kmalloc_array") ||
         hasCalleeName(Call, "kmemdup") ||
         hasCalleeName(Call, "kmemdup_nul") ||
         hasCalleeName(Call, "kstrdup") ||
         hasCalleeName(Call, "kstrndup") ||
         hasCalleeName(Call, "vmalloc") ||
         hasCalleeName(Call, "vzalloc") ||
         hasCalleeName(Call, "kvmalloc") ||
         hasCalleeName(Call, "kvmalloc_array") ||
         hasCalleeName(Call, "kvcalloc") ||
         hasCalleeName(Call, "kvzalloc");
}

bool SAGenTestChecker::isDeallocationCall(const CallEvent &Call) {
  return hasCalleeName(Call, "free") ||
         hasCalleeName(Call, "kfree") ||
         hasCalleeName(Call, "kfree_sensitive") ||
         hasCalleeName(Call, "vfree") ||
         hasCalleeName(Call, "kvfree");
}

const MemRegion *SAGenTestChecker::getBaseRegion(SVal Value) {
  const MemRegion *Region = Value.getAsRegion();
  return Region ? Region->getBaseRegion() : nullptr;
}

const MemRegion *
SAGenTestChecker::getTrackedRegion(SVal Value, ProgramStateRef State) {
  const MemRegion *RawRegion = Value.getAsRegion();
  if (!RawRegion)
    return nullptr;

  // Normally a call argument is already the pointee region. The alias lookup
  // also covers analyzer states where the value is represented by pointer
  // storage that was previously recorded by checkBind().
  if (const MemRegion *const *AliasedRegion =
          State->get<PtrAliasMap>(RawRegion))
    return (*AliasedRegion)->getBaseRegion();

  return RawRegion->getBaseRegion();
}

bool SAGenTestChecker::isFalsePositive(ProgramStateRef State,
                                       const MemRegion *Region) {
  const bool *IsAllocated = State->get<AllocatedRegionMap>(Region);
  const bool *WasReleased = State->get<ReleasedRegionMap>(Region);

  // Do not diagnose unknown ownership. In particular, a deallocator commonly
  // receives its owned object through a function parameter, as in:
  //
  //   void destroy(struct object *obj) { kfree(obj); }
  //
  // The caller may have allocated obj with an allocator the checker does not
  // model, or in a different analyzed frame.
  return (!IsAllocated || !*IsAllocated) && (!WasReleased || !*WasReleased);
}

void SAGenTestChecker::checkPostCall(const CallEvent &Call,
                                     CheckerContext &C) const {
  if (!isAllocationCall(Call))
    return;

  // Use the CallEvent return value rather than source-text inspection. This
  // works for direct calls expressed through macros and avoids matching
  // unrelated expressions that merely contain an allocator's spelling.
  const MemRegion *Region = getBaseRegion(Call.getReturnValue());
  if (!Region)
    return;

  ProgramStateRef State = C.getState();
  State = State->set<AllocatedRegionMap>(Region, true);
  State = State->remove<ReleasedRegionMap>(Region);

  // The return value itself is a useful alias anchor until it is stored.
  State = State->set<PtrAliasMap>(Region, Region);
  C.addTransition(State);
}

void SAGenTestChecker::checkPreCall(const CallEvent &Call,
                                    CheckerContext &C) const {
  if (!isDeallocationCall(Call) || Call.getNumArgs() == 0)
    return;

  ProgramStateRef State = C.getState();
  const MemRegion *Region = getTrackedRegion(Call.getArgSVal(0), State);
  if (!Region)
    return;

  const bool *WasReleased = State->get<ReleasedRegionMap>(Region);
  if (WasReleased && *WasReleased) {
    reportIncorrectFree(Call, C);
    return;
  }

  const bool *IsAllocated = State->get<AllocatedRegionMap>(Region);
  if (!IsAllocated || !*IsAllocated) {
    if (isFalsePositive(State, Region))
      return;

    // This branch is retained for completeness if the state representation is
    // extended later with non-boolean allocation states.
    return;
  }

  // The first release is valid. Preserve an explicit released marker so a
  // later cleanup label freeing the same stale pointer is diagnosable.
  State = State->remove<AllocatedRegionMap>(Region);
  State = State->set<ReleasedRegionMap>(Region, true);
  C.addTransition(State);
}

void SAGenTestChecker::checkBind(SVal Loc, SVal Val, const Stmt *S,
                                 CheckerContext &C) const {
  const MemRegion *Storage = Loc.getAsRegion();
  if (!Storage)
    return;

  ProgramStateRef State = C.getState();

  // Assignment overwrites any previous pointer value stored at this location.
  State = State->remove<PtrAliasMap>(Storage);

  if (const MemRegion *Target = getTrackedRegion(Val, State))
    State = State->set<PtrAliasMap>(Storage, Target);

  C.addTransition(State);
}

void SAGenTestChecker::reportIncorrectFree(const CallEvent &Call,
                                           CheckerContext &C) const {
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT,
      "Incorrect free in error handling: pointer was already released on "
      "this path",
      N);
  Report->addRange(Call.getSourceRange());
  C.emitReport(std::move(Report));
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects error-path cleanup that releases an already released resource",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
