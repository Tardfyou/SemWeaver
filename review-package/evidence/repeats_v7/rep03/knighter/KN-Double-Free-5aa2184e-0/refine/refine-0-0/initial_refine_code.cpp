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
#include "clang/StaticAnalyzer/Core/PathSensitive/MemRegion.h"

using namespace clang;
using namespace ento;
using namespace taint;

// A value of true means that the allocation is currently live.
// A value of false means that the allocation was already released.
REGISTER_MAP_WITH_PROGRAMSTATE(AllocatedRegionMap, const MemRegion *, bool)
REGISTER_MAP_WITH_PROGRAMSTATE(AllocatedSymbolMap, SymbolRef, bool)

// Maps pointer storage locations, such as variables and fields, to the region
// currently stored in them. This is only a fallback for cases where the
// argument SVal still refers to the storage location.
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
  static bool isKnownAllocator(const CallEvent &Call);
  static bool isKnownDeallocator(const CallEvent &Call);

  static const MemRegion *getBaseRegion(SVal Value);
  static const MemRegion *getTrackedRegion(SVal Value,
                                           ProgramStateRef State);

  void reportIncorrectFree(const CallEvent &Call,
                           CheckerContext &C) const;
};

bool SAGenTestChecker::isKnownAllocator(const CallEvent &Call) {
  const IdentifierInfo *II = Call.getCalleeIdentifier();
  if (!II)
    return false;

  llvm::StringRef Name = II->getName();

  // Keep this list restricted to functions whose successful return value owns
  // a dynamically allocated object that can be released by the modeled frees.
  return Name == "kmalloc" ||
         Name == "__kmalloc" ||
         Name == "kzalloc" ||
         Name == "kcalloc" ||
         Name == "kmalloc_array" ||
         Name == "kmalloc_node" ||
         Name == "__kmalloc_node" ||
         Name == "kzalloc_node" ||
         Name == "kmemdup" ||
         Name == "kmemdup_nul" ||
         Name == "vmalloc" ||
         Name == "vzalloc" ||
         Name == "vcalloc" ||
         Name == "kvmalloc" ||
         Name == "kvzalloc" ||
         Name == "kvcalloc" ||
         Name == "kvmalloc_array" ||
         Name == "malloc" ||
         Name == "calloc" ||
         Name == "realloc";
}

bool SAGenTestChecker::isKnownDeallocator(const CallEvent &Call) {
  const IdentifierInfo *II = Call.getCalleeIdentifier();
  if (!II)
    return false;

  llvm::StringRef Name = II->getName();

  return Name == "kfree" ||
         Name == "kfree_sensitive" ||
         Name == "kvfree" ||
         Name == "kvfree_sensitive" ||
         Name == "vfree" ||
         Name == "free";
}

const MemRegion *SAGenTestChecker::getBaseRegion(SVal Value) {
  const MemRegion *MR = Value.getAsRegion();
  return MR ? MR->getBaseRegion() : nullptr;
}

const MemRegion *SAGenTestChecker::getTrackedRegion(
    SVal Value, ProgramStateRef State) {
  const MemRegion *RawRegion = Value.getAsRegion();
  if (!RawRegion)
    return nullptr;

  const MemRegion *BaseRegion = RawRegion->getBaseRegion();
  if (State->get<AllocatedRegionMap>(BaseRegion))
    return BaseRegion;

  // Normally CallEvent supplies the pointee region directly. Retain this
  // fallback for pointer storage values tracked by checkBind().
  if (const MemRegion *const *AliasedRegion =
          State->get<PtrAliasMap>(RawRegion))
    return (*AliasedRegion)->getBaseRegion();

  return BaseRegion;
}

void SAGenTestChecker::checkPostCall(const CallEvent &Call,
                                     CheckerContext &C) const {
  if (!isKnownAllocator(Call))
    return;

  SVal ReturnValue = Call.getReturnValue();
  ProgramStateRef State = C.getState();
  bool StateChanged = false;

  if (const MemRegion *MR = getBaseRegion(ReturnValue)) {
    State = State->set<AllocatedRegionMap>(MR, true);
    StateChanged = true;
  }

  if (SymbolRef Sym = ReturnValue.getAsSymbol()) {
    State = State->set<AllocatedSymbolMap>(Sym, true);
    StateChanged = true;
  }

  if (StateChanged)
    C.addTransition(State);
}

void SAGenTestChecker::checkPreCall(const CallEvent &Call,
                                    CheckerContext &C) const {
  if (!isKnownDeallocator(Call) || Call.getNumArgs() == 0)
    return;

  SVal ArgValue = Call.getArgSVal(0);
  ProgramStateRef State = C.getState();

  const MemRegion *MR = getTrackedRegion(ArgValue, State);
  SymbolRef Sym = ArgValue.getAsSymbol();

  const bool *RegionStatus =
      MR ? State->get<AllocatedRegionMap>(MR) : nullptr;
  const bool *SymbolStatus =
      Sym ? State->get<AllocatedSymbolMap>(Sym) : nullptr;

  // A release is suspicious only when it is proven to target an allocation
  // which this checker has already observed being released. In particular,
  // do not report untracked pointers: they may be caller-owned or returned
  // from a valid allocator wrapper that is not modeled here.
  if ((RegionStatus && !*RegionStatus) ||
      (SymbolStatus && !*SymbolStatus)) {
    reportIncorrectFree(Call, C);
    return;
  }

  // Record the release instead of removing the entry. Retaining the false
  // value lets a later cleanup path prove a double/incorrect free.
  if ((RegionStatus && *RegionStatus) ||
      (SymbolStatus && *SymbolStatus)) {
    if (RegionStatus)
      State = State->set<AllocatedRegionMap>(MR, false);
    if (SymbolStatus)
      State = State->set<AllocatedSymbolMap>(Sym, false);

    C.addTransition(State);
  }
}

void SAGenTestChecker::checkBind(SVal Loc, SVal Val, const Stmt *S,
                                 CheckerContext &C) const {
  const MemRegion *StorageRegion = Loc.getAsRegion();
  if (!StorageRegion)
    return;

  const auto *TypedRegion = dyn_cast<TypedValueRegion>(StorageRegion);
  if (!TypedRegion || !TypedRegion->getValueType()->isPointerType())
    return;

  ProgramStateRef State = C.getState();
  ProgramStateRef NewState = State;

  if (const MemRegion *PointeeRegion = Val.getAsRegion()) {
    NewState = NewState->set<PtrAliasMap>(
        StorageRegion, PointeeRegion->getBaseRegion());
  } else {
    // Clear stale alias information for assignments such as "ptr = NULL".
    NewState = NewState->remove<PtrAliasMap>(StorageRegion);
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
      "Incorrect free in error handling: allocation was already released",
      N);
  Report->addRange(Call.getSourceRange());
  C.emitReport(std::move(Report));
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &Registry) {
  Registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects cleanup paths that release a previously released allocation",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
