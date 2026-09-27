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
#include "clang/AST/Type.h"
#include "clang/Lex/Lexer.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

using namespace clang;
using namespace ento;
using namespace taint;

// Program states
REGISTER_MAP_WITH_PROGRAMSTATE(CompUseMap, const MemRegion*, unsigned)
REGISTER_MAP_WITH_PROGRAMSTATE(CompLastUseStmt, const MemRegion*, const Stmt*)

namespace {

class SAGenTestChecker : public Checker< check::PostCall, check::EndFunction > {
   mutable std::unique_ptr<BugType> BT;

   // TU-wide pairing relation: some submitter in this TU waits on its
   // completion with a timeout (wait_for_completion_timeout), so it may
   // abandon and free the shared context while the worker still runs.
   // Cached per translation unit and derived from the AST, because the
   // worker is defined (and thus analyzed) before the submitter that
   // queues it in adf_aer.c.
   mutable llvm::DenseMap<const TranslationUnitDecl *, bool> TimedWaitInTUCache;

   // Flags for CompUseMap
   static constexpr unsigned SEEN_COMPLETE = 0x1;  // complete(&ctx->compl)
   static constexpr unsigned SEEN_DONE     = 0x4;  // completion_done(&ctx->compl)

   // Per-worker ownership shape cache: true when the worker both signals
   // (complete) and frees (kfree) the same context object on its paths.
   mutable llvm::DenseMap<const FunctionDecl *, bool> SplitOwnershipCache;

   public:
      SAGenTestChecker() : BT(new BugType(this, "Workqueue timed-wait UAF risk", "Concurrency")) {}

      void checkPostCall(const CallEvent &Call, CheckerContext &C) const;
      void checkEndFunction(const ReturnStmt *RS, CheckerContext &C) const;

   private:
      // Helpers
      bool callIs(const CallEvent &Call, StringRef Name, CheckerContext &C) const;
      bool isWorkerFunction(const FunctionDecl *FD) const;
      bool isSplitOwnershipWorker(const FunctionDecl *FD) const;
      bool hasTimedWaitInTU(TranslationUnitDecl *TU) const;

      ProgramStateRef setFlag(ProgramStateRef State, const MemRegion *B, unsigned Flag) const;
      ProgramStateRef setLastUse(ProgramStateRef State, const MemRegion *B, const Stmt *S) const;

      const MemRegion *getContextBaseFromCompletionArg(const Expr *ArgE, CheckerContext &C) const;

      void reportMissingGuard(const MemRegion *B, const Stmt *S, CheckerContext &C) const;
};

// Determine if Call refers to a function with given name using source text.
bool SAGenTestChecker::callIs(const CallEvent &Call, StringRef Name, CheckerContext &C) const {
  const Expr *Origin = Call.getOriginExpr();
  if (!Origin)
    return false;
  return ExprHasName(Origin, Name, C);
}

// Identify worker function by parameter of type 'struct work_struct *'
bool SAGenTestChecker::isWorkerFunction(const FunctionDecl *FD) const {
  if (!FD)
    return false;

  for (const ParmVarDecl *P : FD->parameters()) {
    QualType QT = P->getType();
    if (!QT->isPointerType())
      continue;
    QualType Pointee = QT->getPointeeType();
    if (const RecordType *RT = Pointee->getAs<RecordType>()) {
      const RecordDecl *RD = RT->getDecl();
      IdentifierInfo *II = RD->getIdentifier();
      if (II && II->getName() == "work_struct")
        return true;
    }
  }
  return false;
}

// AST helper: collect the owning local variables of the context objects a
// worker signals (complete(&obj->compl)) and frees (kfree(obj)).
class WorkerOwnershipVisitor
    : public RecursiveASTVisitor<WorkerOwnershipVisitor> {
public:
  llvm::SmallVector<const VarDecl *, 4> Signalled;
  llvm::SmallVector<const VarDecl *, 4> Freed;

  bool VisitCallExpr(CallExpr *CE) {
    const FunctionDecl *Callee = CE->getDirectCallee();
    if (!Callee || CE->getNumArgs() < 1)
      return true;
    StringRef Name = Callee->getName();
    if (Name == "complete") {
      if (const VarDecl *VD = getCompletionOwner(CE->getArg(0)))
        Signalled.push_back(VD);
    } else if (Name == "kfree") {
      if (const VarDecl *VD = getFreedOwner(CE->getArg(0)))
        Freed.push_back(VD);
    }
    return true;
  }

private:
  // Owner local of "&obj->compl": the object whose completion is signalled.
  static const VarDecl *getCompletionOwner(const Expr *E) {
    if (!E)
      return nullptr;
    const auto *UO = dyn_cast<UnaryOperator>(E->IgnoreParenImpCasts());
    if (!UO || UO->getOpcode() != UO_AddrOf)
      return nullptr;
    const auto *ME =
        dyn_cast<MemberExpr>(UO->getSubExpr()->IgnoreParenImpCasts());
    if (!ME)
      return nullptr;
    const auto *Base =
        dyn_cast<DeclRefExpr>(ME->getBase()->IgnoreParenImpCasts());
    return Base ? dyn_cast<VarDecl>(Base->getDecl()) : nullptr;
  }

  // Owner local of the freed pointer expression.
  static const VarDecl *getFreedOwner(const Expr *E) {
    if (!E)
      return nullptr;
    const auto *DRE = dyn_cast<DeclRefExpr>(E->IgnoreParenImpCasts());
    return DRE ? dyn_cast<VarDecl>(DRE->getDecl()) : nullptr;
  }
};

// Does this translation unit contain a timed wait on a completion
// (wait_for_completion_timeout)? Such a submitter can time out, stop
// waiting and exit while the queued worker is still running, abandoning
// the shared context object.
class TimedWaitVisitor : public RecursiveASTVisitor<TimedWaitVisitor> {
public:
  bool Found = false;
  bool VisitCallExpr(CallExpr *CE) {
    const FunctionDecl *Callee = CE->getDirectCallee();
    if (Callee && Callee->getName() == "wait_for_completion_timeout")
      Found = true;
    return !Found;
  }
};

bool SAGenTestChecker::hasTimedWaitInTU(TranslationUnitDecl *TU) const {
  if (!TU)
    return false;
  if (auto It = TimedWaitInTUCache.find(TU); It != TimedWaitInTUCache.end())
    return It->second;
  TimedWaitVisitor V;
  V.TraverseDecl(TU);
  TimedWaitInTUCache[TU] = V.Found;
  return V.Found;
}

// A worker whose body both completes and frees the same context object has
// split ownership of it: on some paths the worker is the owner, on others
// the timed-wait submitter is. Only for such a worker can an unguarded
// complete() race with the submitter's timeout free; a worker that only
// signals (or only frees) the object has a single owner and no race.
bool SAGenTestChecker::isSplitOwnershipWorker(const FunctionDecl *FD) const {
  if (!FD || !FD->hasBody())
    return false;
  if (auto It = SplitOwnershipCache.find(FD); It != SplitOwnershipCache.end())
    return It->second;

  WorkerOwnershipVisitor V;
  V.TraverseStmt(FD->getBody());
  bool Split = false;
  for (const VarDecl *S : V.Signalled) {
    for (const VarDecl *F : V.Freed) {
      if (S == F) {
        Split = true;
        break;
      }
    }
    if (Split)
      break;
  }
  SplitOwnershipCache[FD] = Split;
  return Split;
}

// Update flag bitmask for a given base region
ProgramStateRef SAGenTestChecker::setFlag(ProgramStateRef State, const MemRegion *B, unsigned Flag) const {
  if (!B)
    return State;
  const unsigned *Old = State->get<CompUseMap>(B);
  unsigned NewFlags = (Old ? *Old : 0) | Flag;
  State = State->set<CompUseMap>(B, NewFlags);
  return State;
}

// Record the last interesting statement for a region
ProgramStateRef SAGenTestChecker::setLastUse(ProgramStateRef State, const MemRegion *B, const Stmt *S) const {
  if (!B || !S)
    return State;
  State = State->set<CompLastUseStmt>(B, S);
  return State;
}

// Extract the context base region from an argument expected to be &ctx->compl or a pointer to a completion field.
// Strategy: get region for the arg; if it's a FieldRegion, use its super; otherwise use base.
const MemRegion *SAGenTestChecker::getContextBaseFromCompletionArg(const Expr *ArgE, CheckerContext &C) const {
  if (!ArgE)
    return nullptr;

  ProgramStateRef State = C.getState();
  SVal V = State->getSVal(ArgE, C.getLocationContext());
  const MemRegion *MR = V.getAsRegion();
  if (!MR) {
    MR = getMemRegionFromExpr(ArgE, C);
    if (!MR)
      return nullptr;
  }

  MR = MR->getBaseRegion();
  // If this is a field region (e.g., &ctx->compl), climb to its super region.
  if (const FieldRegion *FR = dyn_cast<FieldRegion>(MR)) {
    const MemRegion *Super = FR->getSuperRegion();
    if (Super)
      return Super->getBaseRegion();
  }
  // Otherwise return the base region as best-effort.
  return MR ? MR->getBaseRegion() : nullptr;
}

void SAGenTestChecker::checkPostCall(const CallEvent &Call, CheckerContext &C) const {
  ProgramStateRef State = C.getState();

  // Track complete(&ctx->compl)
  if (callIs(Call, "complete", C)) {
    const Expr *Arg0 = Call.getArgExpr(0);
    const MemRegion *B = getContextBaseFromCompletionArg(Arg0, C);
    if (B) {
      State = setFlag(State, B, SEEN_COMPLETE);
      State = setLastUse(State, B, Call.getOriginExpr());
      C.addTransition(State);
    }
    return;
  }

  // Track completion_done(&ctx->compl)
  if (callIs(Call, "completion_done", C)) {
    const Expr *Arg0 = Call.getArgExpr(0);
    const MemRegion *B = getContextBaseFromCompletionArg(Arg0, C);
    if (B) {
      State = setFlag(State, B, SEEN_DONE);
      C.addTransition(State);
    }
    return;
  }

}

void SAGenTestChecker::reportMissingGuard(const MemRegion *B, const Stmt *S, CheckerContext &C) const {
  if (!BT)
    return;
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto R = std::make_unique<PathSensitiveBugReport>(
      *BT, "Missing completion_done() guard in worker; submitter with timed wait may free the context", N);
  if (S)
    R->addRange(S->getSourceRange());
  C.emitReport(std::move(R));
}

void SAGenTestChecker::checkEndFunction(const ReturnStmt *RS, CheckerContext &C) const {
  const LocationContext *LCtx = C.getLocationContext();
  const FunctionDecl *FD = dyn_cast_or_null<FunctionDecl>(LCtx ? LCtx->getDecl() : nullptr);
  if (!FD)
    return;

  // Focus on worker functions
  if (!isWorkerFunction(FD))
    return;

  // The paired submitter waits with a timeout in this TU and may free the
  // shared context while the worker is still running. Resolved from the
  // AST rather than from analysis order, so the gate holds whether the
  // analyzer visits the submitter before or after this worker.
  if (!hasTimedWaitInTU(FD->getASTContext().getTranslationUnitDecl()))
    return;

  // Only workers with split ownership (complete() and kfree() on the same
  // context object) race with a timeout-abandoning submitter.
  if (!isSplitOwnershipWorker(FD))
    return;

  ProgramStateRef State = C.getState();

  CompUseMapTy Map = State->get<CompUseMap>();
  for (CompUseMapTy::iterator I = Map.begin(), E = Map.end(); I != E; ++I) {
    const MemRegion *B = I->first;
    unsigned Flags = I->second;

    // Warn when the worker signals the shared completion on a path that never
    // checked completion_done(): the timed-wait submitter may already have
    // freed the context. completion_done()-guarded paths stay silent.
    if ((Flags & SEEN_COMPLETE) != 0 && (Flags & SEEN_DONE) == 0) {
      const Stmt *const *LastPtr = State->get<CompLastUseStmt>(B);
      const Stmt *Last = LastPtr ? *LastPtr : nullptr;
      reportMissingGuard(B, Last, C);
    }
  }
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects missing completion_done() guard in workqueue worker that may race with submitter timeout and free",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
