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
#include "llvm/ADT/SmallVector.h"

using namespace clang;
using namespace ento;
using namespace taint;

//------------------------------------------------------------------------------
// Program state maps:
//
// PossibleNullPtrMap records the base region represented by a pointer returned
// from devm_kasprintf(). The boolean value is unused beyond identifying a
// tracked result; the analyzer's constraint manager determines whether that
// result can still be NULL on the current path.
//
// PtrAliasMap records storage locations that currently contain a tracked
// devm_kasprintf() result. It is useful when the analyzer represents a later
// pointer value through its storage region rather than directly through the
// original symbolic return region.
//------------------------------------------------------------------------------
REGISTER_MAP_WITH_PROGRAMSTATE(PossibleNullPtrMap, const MemRegion *, bool)
REGISTER_MAP_WITH_PROGRAMSTATE(PtrAliasMap, const MemRegion *,
                               const MemRegion *)

namespace {

static bool isDevmKasprintf(const CallEvent &Call) {
  const IdentifierInfo *Callee = Call.getCalleeIdentifier();
  return Callee && Callee->getName() == "devm_kasprintf";
}

/// Return the tracked devm_kasprintf() result represented by V, if any.
static const MemRegion *getTrackedOrigin(ProgramStateRef State, SVal V) {
  const MemRegion *RawRegion = V.getAsRegion();
  if (!RawRegion)
    return nullptr;

  if (State->get<PossibleNullPtrMap>(RawRegion))
    return RawRegion;

  const MemRegion *BaseRegion = RawRegion->getBaseRegion();
  if (BaseRegion && State->get<PossibleNullPtrMap>(BaseRegion))
    return BaseRegion;

  if (const auto *Alias = State->get<PtrAliasMap>(RawRegion))
    return *Alias;

  if (BaseRegion) {
    if (const auto *Alias = State->get<PtrAliasMap>(BaseRegion))
      return *Alias;
  }

  return nullptr;
}

/// Return the tracked origin for a location that is being dereferenced.
///
/// This intentionally does not consult PtrAliasMap. A load of the local
/// pointer variable `name` accesses its VarRegion; that is not a dereference
/// of the returned allocation. A real dereference accesses the symbolic
/// region returned by devm_kasprintf(), or one of its subregions.
static const MemRegion *getDereferencedTrackedOrigin(ProgramStateRef State,
                                                     SVal Loc) {
  const MemRegion *AccessedRegion = Loc.getAsRegion();
  if (!AccessedRegion)
    return nullptr;

  if (State->get<PossibleNullPtrMap>(AccessedRegion))
    return AccessedRegion;

  const MemRegion *BaseRegion = AccessedRegion->getBaseRegion();
  if (BaseRegion && State->get<PossibleNullPtrMap>(BaseRegion))
    return BaseRegion;

  return nullptr;
}

/// Ask the constraint manager whether Origin can be NULL on this path.
///
/// This avoids a path-insensitive "checked" bit. For example, after
///
///   if (!name)
///     return -ENOMEM;
///
/// the surviving path constrains name to be non-NULL, while a path that
/// continues after `if (!name) log_error();` still permits name to be NULL.
static bool canBeNull(ProgramStateRef State, const MemRegion *Origin) {
  if (!Origin)
    return false;

  SVal PointerValue = loc::MemRegionVal(Origin);
  if (auto DefinedValue = PointerValue.getAs<DefinedOrUnknownSVal>())
    return State->assume(*DefinedValue, false) != nullptr;

  // If the value cannot be represented as a constrained value, remain
  // conservative and treat it as potentially NULL.
  return true;
}

/// Determine whether assigning a pointer to Storage makes it escape from the
/// local expression. Local pointer-to-pointer aliases are handled by the
/// alias map and are not reports by themselves.
static bool isEscapingPointerStorage(const MemRegion *Storage) {
  if (!Storage)
    return false;

  if (isa<FieldRegion>(Storage) || isa<ElementRegion>(Storage))
    return true;

  if (const auto *VR = dyn_cast<VarRegion>(Storage))
    return VR->getDecl()->hasGlobalStorage();

  return false;
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
  void reportUncheckedUse(const Stmt *S, CheckerContext &C) const;
};

/// Track the symbolic region returned by devm_kasprintf().
void SAGenTestChecker::checkPostCall(const CallEvent &Call,
                                     CheckerContext &C) const {
  if (!isDevmKasprintf(Call))
    return;

  const MemRegion *ReturnRegion = Call.getReturnValue().getAsRegion();
  if (!ReturnRegion)
    return;

  ReturnRegion = ReturnRegion->getBaseRegion();
  if (!ReturnRegion)
    return;

  ProgramStateRef State = C.getState();
  State = State->set<PossibleNullPtrMap>(ReturnRegion, true);
  C.addTransition(State);
}

/// Diagnose calls whose callee is explicitly known to dereference one of its
/// pointer arguments. This avoids treating arbitrary pointer passing as a
/// dereference while still handling modeled library/kernel helper functions.
void SAGenTestChecker::checkPreCall(const CallEvent &Call,
                                    CheckerContext &C) const {
  llvm::SmallVector<unsigned, 4> DerefParams;
  if (!functionKnownToDeref(Call, DerefParams))
    return;

  ProgramStateRef State = C.getState();

  for (unsigned ParamIndex : DerefParams) {
    if (ParamIndex >= Call.getNumArgs())
      continue;

    const MemRegion *Origin =
        getTrackedOrigin(State, Call.getArgSVal(ParamIndex));
    if (Origin && canBeNull(State, Origin)) {
      reportUncheckedUse(Call.getOriginExpr(), C);
      return;
    }
  }
}

/// Preserve aliases without marking the destination as an unchecked
/// dereference target. This distinction prevents `if (!name)` from being
/// reported merely because reading `name` loads a tracked pointer value.
///
/// An assignment into a field, array element, or global storage is an escape.
/// The supplied buggy code performs exactly such an escape through:
///
///   aux_driver->name = name;
///   aux_dev->name = name;
///
/// The repaired code reaches those assignments only on a path where `name` is
/// constrained non-NULL by the preceding guard.
void SAGenTestChecker::checkBind(SVal Loc, SVal Val, const Stmt *StoreE,
                                 CheckerContext &C) const {
  const MemRegion *Storage = Loc.getAsRegion();
  if (!Storage)
    return;

  ProgramStateRef State = C.getState();
  const MemRegion *Origin = getTrackedOrigin(State, Val);

  if (!Origin) {
    State = State->remove<PtrAliasMap>(Storage);
    C.addTransition(State);
    return;
  }

  State = State->set<PtrAliasMap>(Storage, Origin);

  if (isEscapingPointerStorage(Storage) && canBeNull(State, Origin))
    reportUncheckedUse(StoreE, C);

  C.addTransition(State);
}

/// Report only actual accesses through the returned pointer or one of its
/// subregions. Loading the pointer object itself, including the load needed by
/// `if (!name)`, has a VarRegion base and is intentionally ignored.
void SAGenTestChecker::checkLocation(SVal Loc, bool IsLoad, const Stmt *S,
                                     CheckerContext &C) const {
  ProgramStateRef State = C.getState();
  const MemRegion *Origin = getDereferencedTrackedOrigin(State, Loc);

  if (Origin && canBeNull(State, Origin))
    reportUncheckedUse(S, C);
}

void SAGenTestChecker::reportUncheckedUse(const Stmt *S,
                                          CheckerContext &C) const {
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT, "Unchecked devm_kasprintf return value used", N);

  if (S)
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
