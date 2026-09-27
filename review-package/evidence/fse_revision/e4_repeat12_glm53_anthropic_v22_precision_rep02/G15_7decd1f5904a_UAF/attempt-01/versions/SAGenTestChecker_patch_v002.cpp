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
#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Type.h"

using namespace clang;
using namespace ento;
using namespace taint;

//================ Program state customizations ================

// Tracks base regions (of pointed objects) that have been released,
// mapped to the name of the releasing function (string literal).
// An ownership transition (e.g. mptcp_close_ssk) retires only the
// pointed-to object's region: the storage of local pointer variables
// stays valid, so re-assigning or reading such a variable after the
// release is not a use-after-free.  Loads through any pointer bound to
// the retired region still resolve to the same base region.
REGISTER_MAP_WITH_PROGRAMSTATE(ReleasedMap, const MemRegion*, const char*)

namespace {

// Helper table for functions that may release/free certain pointer params.
struct KnownReleaseFunction {
  const char *Name;
  llvm::SmallVector<unsigned, 4> Params; // 0-based indices of params that are released/freed
};

// Release contracts: each entry names a function that performs a complete
// ownership transition on the caller's object for the listed parameter, after
// which the caller must not dereference that object.  mptcp_close_ssk()
// tears down and disposes of its third argument (the subflow context), so any
// later load through that region is a use-after-free.  Generic deallocator
// names such as kfree/kvfree are deliberately not modeled: the freed node is
// retired via linked-list mutations the analyzer cannot follow, so marking
// bare kfree releases only produced unprovable use-after-free reports in
// unrelated list teardown loops instead of a post-transition dereference.
static const KnownReleaseFunction ReleaseTable[] = {
  {"mptcp_close_ssk", {2}}, // third parameter (subflow) is released/teardown
};

// Forward declarations of helpers
static const MemRegion *getBaseRegionFromExpr(const Expr *E, CheckerContext &C);
static const MemRegion *getBaseFromLoc(SVal Loc);
static bool functionKnownToRelease(const CallEvent &Call,
                                   CheckerContext &C,
                                   llvm::SmallVectorImpl<unsigned> &FreedParams,
                                   const char* &FnNameOut);
static bool functionKnownToDeref(const CallEvent &Call,
                                 llvm::SmallVectorImpl<unsigned> &DerefParams);

static ProgramStateRef markReleased(ProgramStateRef State,
                                    const MemRegion *R,
                                    const char *FnName);

class SAGenTestChecker
  : public Checker<
      check::PostCall,
      check::PreCall,
      check::Location> {
   mutable std::unique_ptr<BugType> BT;

   public:
      SAGenTestChecker() : BT(new BugType(this, "Use-after-free", "Memory Management")) {}

      void checkPostCall(const CallEvent &Call, CheckerContext &C) const;
      void checkPreCall(const CallEvent &Call, CheckerContext &C) const;
      void checkLocation(SVal Loc, bool IsLoad, const Stmt *S, CheckerContext &C) const;

   private:
      void reportUAF(const Stmt *S, const char *ByFn, StringRef Detail, CheckerContext &C) const;
};

//================ Helper implementations ================

static const MemRegion *getBaseRegionFromExpr(const Expr *E, CheckerContext &C) {
  if (!E)
    return nullptr;
  const MemRegion *MR = getMemRegionFromExpr(E, C);
  if (!MR)
    return nullptr;
  MR = MR->getBaseRegion();
  return MR;
}

static const MemRegion *getBaseFromLoc(SVal Loc) {
  if (const MemRegion *MR = Loc.getAsRegion()) {
    return MR->getBaseRegion();
  }
  return nullptr;
}

static bool functionKnownToRelease(const CallEvent &Call,
                                   CheckerContext &C,
                                   llvm::SmallVectorImpl<unsigned> &FreedParams,
                                   const char* &FnNameOut) {
  const Expr *Origin = Call.getOriginExpr();
  if (!Origin)
    return false;

  for (const auto &Entry : ReleaseTable) {
    if (ExprHasName(Origin, Entry.Name, C)) {
      FreedParams.append(Entry.Params.begin(), Entry.Params.end());
      FnNameOut = Entry.Name;
      return true;
    }
  }
  return false;
}

// Minimal helper: recognize a few common library functions that dereference arguments.
static bool functionKnownToDeref(const CallEvent &Call,
                                 llvm::SmallVectorImpl<unsigned> &DerefParams) {
  auto *II = Call.getCalleeIdentifier();
  if (!II)
    return false;
  StringRef Name = II->getName();
  if (Name == "memcpy" || Name == "memmove" || Name == "strcpy" || Name == "strncpy") {
    DerefParams.push_back(0);
    DerefParams.push_back(1);
    return true;
  }
  if (Name == "strlen" || Name == "strcmp") {
    DerefParams.push_back(0);
    return true;
  }
  return false;
}

// The releasing call performs a complete ownership transition on the
// pointed-to object, so only that object's base region is retired.
// Pointer-variable storage is not an alias of the freed object (binding
// a local pointer to a region does not free the variable itself), and
// value-level aliasing is already covered because every load through a
// pointer bound to the retired region resolves to the same base region.
static ProgramStateRef markReleased(ProgramStateRef State,
                                    const MemRegion *R,
                                    const char *FnName) {
  if (!R || !FnName)
    return State;

  return State->set<ReleasedMap>(R->getBaseRegion(), FnName);
}

//================ Checker logic ================

void SAGenTestChecker::checkPostCall(const CallEvent &Call, CheckerContext &C) const {
  ProgramStateRef State = C.getState();

  llvm::SmallVector<unsigned, 4> FreedParams;
  const char *FnName = nullptr;
  if (!functionKnownToRelease(Call, C, FreedParams, FnName))
    return;

  for (unsigned Idx : FreedParams) {
    if (Idx >= Call.getNumArgs())
      continue;

    const Expr *ArgE = Call.getArgExpr(Idx);
    const MemRegion *Base = getBaseRegionFromExpr(ArgE, C);
    if (!Base)
      continue;

    State = markReleased(State, Base, FnName);
  }

  if (State)
    C.addTransition(State);
}

void SAGenTestChecker::checkPreCall(const CallEvent &Call, CheckerContext &C) const {
  ProgramStateRef State = C.getState();

  llvm::SmallVector<unsigned, 4> DerefParams;
  if (!functionKnownToDeref(Call, DerefParams))
    return;

  for (unsigned Idx : DerefParams) {
    if (Idx >= Call.getNumArgs())
      continue;

    const Expr *ArgE = Call.getArgExpr(Idx);
    const MemRegion *Base = getBaseRegionFromExpr(ArgE, C);
    if (!Base)
      continue;

    const char *const *ReleasedBy = State->get<ReleasedMap>(Base);
    if (ReleasedBy && *ReleasedBy) {
      // Report UAF at call site: passing a released pointer to a function that dereferences it.
      reportUAF(Call.getOriginExpr(), *ReleasedBy,
                "passing released pointer to a function that dereferences it", C);
      return;
    }
  }
}

void SAGenTestChecker::checkLocation(SVal Loc, bool IsLoad, const Stmt *S, CheckerContext &C) const {
  ProgramStateRef State = C.getState();

  const MemRegion *Base = getBaseFromLoc(Loc);
  if (!Base)
    return;

  const char *const *ReleasedBy = State->get<ReleasedMap>(Base);
  if (!ReleasedBy || !*ReleasedBy)
    return;

  // Use-after-free detected on memory access (load/store).
  reportUAF(S, *ReleasedBy, "pointer used after it was released", C);
}

void SAGenTestChecker::reportUAF(const Stmt *S, const char *ByFn, StringRef Detail, CheckerContext &C) const {
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  SmallString<128> Msg;
  Msg += "use-after-free: ";
  if (!Detail.empty()) {
    Msg += Detail;
    Msg += "; ";
  }
  Msg += "released by call to ";
  Msg += ByFn;

  auto R = std::make_unique<PathSensitiveBugReport>(*BT, Msg.str(), N);
  if (S)
    R->addRange(S->getSourceRange());
  C.emitReport(std::move(R));
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects use-after-free when a pointer is used after a function may release it (e.g., mptcp_close_ssk)",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
