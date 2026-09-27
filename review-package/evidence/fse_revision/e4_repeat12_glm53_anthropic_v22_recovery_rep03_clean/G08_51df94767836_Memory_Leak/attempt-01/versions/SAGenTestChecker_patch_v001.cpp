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
#include "clang/AST/Stmt.h"
#include "clang/AST/Expr.h"
#include "llvm/ADT/StringExtras.h"

using namespace clang;
using namespace ento;
using namespace taint;

// Program state maps
// Tracks the pointer region that owns an allocated net_device in current loop iteration.
REGISTER_MAP_WITH_PROGRAMSTATE(PendingNetdevMap, const MemRegion*, const Stmt*)
// Tracks the loop statement where the allocation happened (For/While/Do).
REGISTER_MAP_WITH_PROGRAMSTATE(NetdevLoopMap, const MemRegion*, const Stmt*)
// Pointer aliasing: dest region -> source region.
REGISTER_MAP_WITH_PROGRAMSTATE(PtrAliasMap, const MemRegion*, const MemRegion*)

namespace {

// Simple visitor to collect all CallExpr inside a statement subtree.
struct CallCollectorVisitor : public RecursiveASTVisitor<CallCollectorVisitor> {
  SmallVector<const CallExpr*, 16> Calls;
  bool VisitCallExpr(CallExpr *CE) {
    Calls.push_back(CE);
    return true;
  }
};

// Simple visitor to collect all GotoStmt inside a statement subtree.
struct GotoCollectorVisitor : public RecursiveASTVisitor<GotoCollectorVisitor> {
  SmallVector<const GotoStmt*, 8> Gotos;
  bool VisitGotoStmt(GotoStmt *GS) {
    Gotos.push_back(GS);
    return true;
  }
};

// Locates the labeled statement that a goto target resolves to.
struct LabelCollectorVisitor : public RecursiveASTVisitor<LabelCollectorVisitor> {
  const LabelDecl *Target = nullptr;
  const LabelStmt *Found = nullptr;
  bool VisitLabelStmt(LabelStmt *LS) {
    if (Target && LS->getDecl() == Target && !Found)
      Found = LS;
    return true;
  }
};

class SAGenTestChecker
  : public Checker<
        check::BeginFunction,
        check::Bind,
        check::PostCall,
        check::BranchCondition> {
   mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Resource leak in loop iteration (net_device)",
                       "Memory Management")) {}

  void checkBeginFunction(CheckerContext &C) const;
  void checkBind(SVal Loc, SVal Val, const Stmt *StoreE, CheckerContext &C) const;
  void checkPostCall(const CallEvent &Call, CheckerContext &C) const;
  void checkBranchCondition(const Stmt *Condition, CheckerContext &C) const;

private:
  // Helpers
  static bool isAllocNetdevCall(const Expr *E, CheckerContext &C);
  static bool isExitLikeLabel(const LabelDecl *LD);
  static const Stmt* findEnclosingLoop(const Stmt *S, CheckerContext &C);
  static bool stmtWithinLoop(const Stmt *S, CheckerContext &C, const Stmt *LoopS);

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

  static bool thenContainsFreeOfRegion(const Stmt *Then, ProgramStateRef State,
                                       const MemRegion *TargetR,
                                       CheckerContext &C) {
    if (!Then || !TargetR)
      return false;

    const MemRegion *CanonTarget = canonicalizeRegion(State, TargetR);
    if (!CanonTarget)
      return false;

    CallCollectorVisitor V;
    V.TraverseStmt(const_cast<Stmt *>(Then));
    for (const CallExpr *CE : V.Calls) {
      if (!CE) continue;
      if (!ExprHasName(CE, "free_netdev", C))
        continue;
      if (CE->getNumArgs() < 1)
        continue;
      const Expr *Arg0 = CE->getArg(0);
      if (!Arg0) continue;
      const MemRegion *ArgR = nullptr;
      SVal ArgV = State->getSVal(Arg0, C.getLocationContext());
      if (!ArgV.isUnknownOrUndef())
        ArgR = ArgV.getAsRegion();
      if (!ArgR)
        ArgR = getMemRegionFromExpr(Arg0, C);
      if (!ArgR) continue;
      ArgR = canonicalizeRegion(State, ArgR->getBaseRegion());
      if (ArgR && ArgR == CanonTarget)
        return true;
    }
    return false;
  }

  void erasePendingFor(ProgramStateRef &State, const MemRegion *R) const {
    if (!R) return;
    R = R->getBaseRegion();
    State = State->remove<PendingNetdevMap>(R);
    State = State->remove<NetdevLoopMap>(R);
    // Optional: could clear aliases for R as a key
    auto AM = State->get<PtrAliasMap>();
    if (!AM.isEmpty()) {
      for (auto It = AM.begin(), E = AM.end(); It != E; ++It) {
        if (It->first == R) {
          State = State->remove<PtrAliasMap>(It->first);
        }
      }
    }
  }
};

void SAGenTestChecker::checkBeginFunction(CheckerContext &C) const {
  // Only reset ownership tracking at a top-level function entry; inlined
  // frames share the caller's path state and must not discard it mid-loop.
  if (!C.inTopFrame())
    return;

  ProgramStateRef State = C.getState();

  // Clear all maps to avoid cross-function bleed.
  auto PM = State->get<PendingNetdevMap>();
  if (!PM.isEmpty()) {
    for (auto I = PM.begin(), E = PM.end(); I != E; ++I) {
      State = State->remove<PendingNetdevMap>(I->first);
    }
  }
  auto LM = State->get<NetdevLoopMap>();
  if (!LM.isEmpty()) {
    for (auto I = LM.begin(), E = LM.end(); I != E; ++I) {
      State = State->remove<NetdevLoopMap>(I->first);
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

// Pointer-likeness is implied by storing a region value; no separate check.

void SAGenTestChecker::checkBind(SVal Loc, SVal Val, const Stmt *StoreE,
                                 CheckerContext &C) const {
  ProgramStateRef State = C.getState();
  bool Changed = false;

  // Track pointer-variable aliasing: ptr = otherRegion. Only a store to the
  // whole variable defines its pointer value; stores into subobjects
  // (ndev->field = ...) must not create aliases on an allocation base.
  if (const auto *VR = dyn_cast_or_null<VarRegion>(Loc.getAsRegion())) {
    if (const MemRegion *SrcR = Val.getAsRegion()) {
      SrcR = SrcR->getBaseRegion();
      if (SrcR) {
        State = State->set<PtrAliasMap>(VR, SrcR);
        Changed = true;
      }
    }
  }

  // Allocation ownership is tracked from the allocation call itself in
  // checkPostCall, keyed by the returned heap region, so that detection does
  // not depend on the shape of the store expression. This callback only
  // maintains dest->source aliases used to resolve free/register arguments.
  (void)StoreE;

  if (Changed)
    C.addTransition(State);
}

void SAGenTestChecker::checkPostCall(const CallEvent &Call, CheckerContext &C) const {
  ProgramStateRef State = C.getState();
  const Expr *Origin = Call.getOriginExpr();
  if (!Origin)
    return;

  // Allocation site: an alloc_netdev-family call inside a loop starts
  // ownership of the returned net_device; track its heap region for this
  // loop iteration.
  if (isAllocNetdevCall(Origin, C)) {
    SVal RetV = Call.getReturnValue();
    if (const MemRegion *AllocR = RetV.getAsRegion()) {
      AllocR = AllocR->getBaseRegion();
      if (const Stmt *LoopS = findEnclosingLoop(Origin, C)) {
        ProgramStateRef NewState = State->set<PendingNetdevMap>(AllocR, Origin);
        NewState = NewState->set<NetdevLoopMap>(AllocR, LoopS);
        C.addTransition(NewState);
      }
    }
    return;
  }

  // If register_netdev(arg0) is called, the ownership is now transferred; stop tracking.
  if (ExprHasName(Origin, "register_netdev", C)) {
    if (Call.getNumArgs() >= 1) {
      const Expr *Arg0 = Call.getArgExpr(0);
      if (Arg0) {
        const MemRegion *ArgR = Call.getArgSVal(0).getAsRegion();
        if (!ArgR)
          ArgR = getMemRegionFromExpr(Arg0, C);
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
        const MemRegion *ArgR = Call.getArgSVal(0).getAsRegion();
        if (!ArgR)
          ArgR = getMemRegionFromExpr(Arg0, C);
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
