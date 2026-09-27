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
REGISTER_MAP_WITH_PROGRAMSTATE(ReleasedMap, const MemRegion*, const char*)

namespace {

// Helper table for functions that may release/free certain pointer params.
struct KnownReleaseFunction {
  const char *Name;
  llvm::SmallVector<unsigned, 4> Params; // 0-based indices of params that are released/freed
};

static const KnownReleaseFunction ReleaseTable[] = {
  {"kfree", {0}},
  {"kvfree", {0}},
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

static ProgramStateRef markReleased(ProgramStateRef State,
                                    const MemRegion *R,
                                    const char *FnName) {
  if (!R || !FnName)
    return State;

  // Freed status is a property of the pointed-to object's base region only.
  // A pointer variable that merely held the address is not itself freed:
  // when a loop iterator (e.g. list_for_each_entry_safe) is reassigned to
  // the next object, that object must not inherit released status, which
  // was the source of fixed-side false positives after kfree.
  const MemRegion *Base = R->getBaseRegion();
  const MemSpaceRegion *Space = Base->getMemorySpace();
  if (isa<StackSpaceRegion>(Space) || isa<GlobalsSpaceRegion>(Space))
    return State;
  return State->set<ReleasedMap>(Base, FnName);
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
