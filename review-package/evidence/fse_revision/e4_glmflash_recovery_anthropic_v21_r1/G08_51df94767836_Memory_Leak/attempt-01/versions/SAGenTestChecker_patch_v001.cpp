#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Checkers/Taint.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ConstraintManager.h"
#include "llvm/ADT/APSInt.h"
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
#include "clang/AST/Stmt.h"
#include "clang/AST/Expr.h"
#include "llvm/ADT/StringExtras.h"

using namespace clang;
using namespace ento;
using namespace taint;

// Program state maps
// Tracks the statement that assigned an allocated net_device to a region on
// the current path (used as the report range).
REGISTER_MAP_WITH_PROGRAMSTATE(PendingNetdevMap, const MemRegion*, const Stmt*)
// Tracks the pointer symbol produced by the net_device allocation for a
// tracked region, so a null constraint (failed allocation) can be separated
// from a live, still-owned allocation.
REGISTER_MAP_WITH_PROGRAMSTATE(PendingNetdevSymMap, const MemRegion*, SymbolRef)
// Pointer aliasing: dest region -> source region.
REGISTER_MAP_WITH_PROGRAMSTATE(PtrAliasMap, const MemRegion*, const MemRegion*)

namespace {

class SAGenTestChecker
  : public Checker<
        check::BeginFunction,
        check::Bind,
        check::PostCall,
        check::EndFunction> {
   mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Resource leak: unreleased net_device on error path",
                       "Memory Management")) {}

  void checkBeginFunction(CheckerContext &C) const;
  void checkBind(SVal Loc, SVal Val, const Stmt *StoreE, CheckerContext &C) const;
  void checkPostCall(const CallEvent &Call, CheckerContext &C) const;
  void checkEndFunction(const ReturnStmt *RS, CheckerContext &C) const;

private:
  // Helpers
  static bool isAllocNetdevCall(const Expr *E, CheckerContext &C);

  static const MemRegion* canonicalizeRegion(ProgramStateRef State,
                                             const MemRegion *R) {
    if (!R) return nullptr;
    R = R->getBaseRegion();
    // Follow aliases a few steps to reach the original source.
    for (int i = 0; i < 8 && R; ++i) {
      if (const MemRegion *const *R2 = State->get<PtrAliasMap>(R))
        R = (*R2)->getBaseRegion();
      else
        break;
    }
    return R;
  }

  // If a register_netdev/free_netdev call denotes a tracked net_device (same
  // allocation symbol or same canonical region), ownership of that object
  // leaves this path; drop the tracking entries.
  static void erasePendingForArgument(ProgramStateRef &State,
                                      const CallEvent &Call,
                                      CheckerContext &C) {
    if (Call.getNumArgs() < 1)
      return;

    SymbolRef ArgSym = Call.getArgSVal(0).getAsLocSymbol();
    const MemRegion *ArgR = nullptr;
    if (const Expr *Arg0 = Call.getArgExpr(0))
      ArgR = getMemRegionFromExpr(Arg0, C);
    if (ArgR)
      ArgR = canonicalizeRegion(State, ArgR->getBaseRegion());
    if (!ArgSym && !ArgR)
      return;

    auto Pend = State->get<PendingNetdevMap>();
    for (auto I = Pend.begin(), E = Pend.end(); I != E; ++I) {
      const MemRegion *R = I->first;
      if (!R)
        continue;
      bool Match = false;
      if (ArgSym) {
        if (const SymbolRef *AllocSym = State->get<PendingNetdevSymMap>(R))
          Match = (*AllocSym == ArgSym);
      }
      if (!Match && ArgR) {
        const MemRegion *Canon = canonicalizeRegion(State, R);
        Match = (Canon && Canon == ArgR);
      }
      if (Match) {
        State = State->remove<PendingNetdevMap>(R);
        State = State->remove<PendingNetdevSymMap>(R);
      }
    }
  }
};

void SAGenTestChecker::checkBeginFunction(CheckerContext &C) const {
  ProgramStateRef State = C.getState();

  // Clear all maps to avoid cross-function bleed.
  auto PM = State->get<PendingNetdevMap>();
  if (!PM.isEmpty()) {
    for (auto I = PM.begin(), E = PM.end(); I != E; ++I) {
      State = State->remove<PendingNetdevMap>(I->first);
    }
  }
  auto SM = State->get<PendingNetdevSymMap>();
  if (!SM.isEmpty()) {
    for (auto I = SM.begin(), E = SM.end(); I != E; ++I) {
      State = State->remove<PendingNetdevSymMap>(I->first);
    }
  }
  auto AM = State->get<PtrAliasMap>();
  if (!AM.isEmpty()) {
    for (auto I = AM.begin(), E = AM.end(); I != E; ++I) {
      State = State->remove<PtrAliasMap>(I->first);
    }
  }

  C.addTransition(State);
}

static bool isPointerLikeRegion(const MemRegion *R) {
  if (!R) return false;
  // We don't strictly need to check type; alias map can accept any region keys.
  // Keep it permissive.
  return true;
}

void SAGenTestChecker::checkBind(SVal Loc, SVal Val, const Stmt *StoreE,
                                 CheckerContext &C) const {
  ProgramStateRef State = C.getState();
  bool Changed = false;

  const MemRegion *DstR = Loc.getAsRegion();
  if (DstR) DstR = DstR->getBaseRegion();

  // Track aliasing: DstR = ValRegion;
  if (DstR && isPointerLikeRegion(DstR)) {
    if (const MemRegion *SrcR = Val.getAsRegion()) {
      SrcR = SrcR->getBaseRegion();
      if (SrcR) {
        State = State->set<PtrAliasMap>(DstR, SrcR);
        Changed = true;
      }
    }
  }

  // Detect net_device allocation assignment: ptr = alloc_etherdev/alloc_netdev...
  // Track the assigning statement for reporting and the allocation's returned
  // pointer symbol, so a later null constraint (failed allocation) is not
  // treated as a live, still-owned object.
  if (StoreE && DstR) {
    const CallExpr *CE = findSpecificTypeInChildren<CallExpr>(StoreE);
    if (CE && isAllocNetdevCall(CE, C)) {
      if (SymbolRef AllocSym = Val.getAsLocSymbol())
        State = State->set<PendingNetdevSymMap>(DstR, AllocSym);
      State = State->set<PendingNetdevMap>(DstR, StoreE);
      Changed = true;
    }
  }

  if (Changed)
    C.addTransition(State);
}

void SAGenTestChecker::checkPostCall(const CallEvent &Call, CheckerContext &C) const {
  ProgramStateRef State = C.getState();
  const Expr *Origin = Call.getOriginExpr();
  if (!Origin)
    return;

  // If register_netdev(arg0) is called, the ownership is now transferred; stop tracking.
  if (ExprHasName(Origin, "register_netdev", C)) {
    if (Call.getNumArgs() >= 1) {
      const Expr *Arg0 = Call.getArgExpr(0);
      if (Arg0) {
        const MemRegion *ArgR = getMemRegionFromExpr(Arg0, C);
        if (ArgR) {
          ArgR = canonicalizeRegion(State, ArgR->getBaseRegion());
          if (ArgR) {
            auto LoopPtr = State->get<NetdevLoopMap>(ArgR);
            auto AllocPtr = State->get<PendingNetdevMap>(ArgR);
            if (LoopPtr || AllocPtr) {
              erasePendingFor(State, ArgR);
              C.addTransition(State);
            }
          }
        }
      }
    }
    return;
  }

  // If free_netdev(arg0) is called, remove from pending if tracked.
  if (ExprHasName(Origin, "free_netdev", C)) {
    if (Call.getNumArgs() >= 1) {
      const Expr *Arg0 = Call.getArgExpr(0);
      if (Arg0) {
        const MemRegion *ArgR = getMemRegionFromExpr(Arg0, C);
        if (ArgR) {
          ArgR = canonicalizeRegion(State, ArgR->getBaseRegion());
          if (ArgR) {
            auto LoopPtr = State->get<NetdevLoopMap>(ArgR);
            auto AllocPtr = State->get<PendingNetdevMap>(ArgR);
            if (LoopPtr || AllocPtr) {
              erasePendingFor(State, ArgR);
              C.addTransition(State);
            }
          }
        }
      }
    }
    return;
  }
}

void SAGenTestChecker::checkBranchCondition(const Stmt *Condition, CheckerContext &C) const {
  if (!Condition)
    return;

  // Find the IfStmt that owns this condition.
  const IfStmt *IS = findSpecificTypeInParents<IfStmt>(Condition, C);
  if (!IS)
    return;

  const Stmt *Then = IS->getThen();
  if (!Then)
    return;

  // Does the then-branch contain a goto to an "exit-like" label?
  GotoCollectorVisitor GV;
  GV.TraverseStmt(const_cast<Stmt *>(Then));

  bool HasExitLikeGoto = false;
  for (const GotoStmt *GS : GV.Gotos) {
    if (!GS) continue;
    const LabelDecl *LD = GS->getLabel();
    if (isExitLikeLabel(LD)) {
      HasExitLikeGoto = true;
      break;
    }
  }
  if (!HasExitLikeGoto)
    return;

  // Find enclosing loop of this if-statement.
  const Stmt *LoopS = findEnclosingLoop(IS, C);
  if (!LoopS)
    return;

  ProgramStateRef State = C.getState();
  auto Pend = State->get<PendingNetdevMap>();
  if (Pend.isEmpty())
    return;

  // For each pending netdev tied to this loop, ensure Then frees it before goto.
  for (auto I = Pend.begin(), E = Pend.end(); I != E; ++I) {
    const MemRegion *R = I->first;
    if (!R) continue;
    R = R->getBaseRegion();

    auto RLoop = State->get<NetdevLoopMap>(R);
    if (!RLoop || *RLoop != LoopS)
      continue;

    // Check whether Then frees the current iteration's net_device.
    if (!thenContainsFreeOfRegion(Then, State, R, C)) {
      ExplodedNode *N = C.generateNonFatalErrorNode();
      if (!N)
        return;

      auto Rpt = std::make_unique<PathSensitiveBugReport>(
          *BT, "Missing free_netdev before goto exit; leaks current net_device",
          N);
      // Try to highlight the 'then' branch range.
      Rpt->addRange(Then->getSourceRange());
      C.emitReport(std::move(Rpt));
      // Do not break; potentially multiple regions (though uncommon).
    }
  }
}

// Helper implementations

bool SAGenTestChecker::isAllocNetdevCall(const Expr *E, CheckerContext &C) {
  if (!E) return false;
  // Recognize common Linux netdev allocation helpers.
  return ExprHasName(E, "alloc_etherdev", C) ||
         ExprHasName(E, "alloc_etherdev_mqs", C) ||
         ExprHasName(E, "alloc_netdev", C) ||
         ExprHasName(E, "alloc_netdev_mqs", C);
}

bool SAGenTestChecker::isExitLikeLabel(const LabelDecl *LD) {
  if (!LD) return false;
  StringRef Name = LD->getName();
  if (Name.empty()) return false;

  std::string Lower = Name.lower();
  StringRef LRef(Lower);

  if (LRef == "exit" || LRef == "out" || LRef == "error")
    return true;

  // Common kernel styles: err, err_X, error_X, out_X
  if (LRef.startswith("err") || LRef.startswith("error") || LRef.startswith("out"))
    return true;

  return false;
}

const Stmt* SAGenTestChecker::findEnclosingLoop(const Stmt *S, CheckerContext &C) {
  if (!S) return nullptr;
  if (const auto *FS = findSpecificTypeInParents<ForStmt>(S, C))
    return FS;
  if (const auto *WS = findSpecificTypeInParents<WhileStmt>(S, C))
    return WS;
  if (const auto *DS = findSpecificTypeInParents<DoStmt>(S, C))
    return DS;
  return nullptr;
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects missing free_netdev before goto exit in loops (leaks current iteration net_device)",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
