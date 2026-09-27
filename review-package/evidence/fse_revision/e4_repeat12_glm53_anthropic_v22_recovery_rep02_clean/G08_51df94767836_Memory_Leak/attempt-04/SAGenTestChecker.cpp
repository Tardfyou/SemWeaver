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
#include "clang/Analysis/AnalysisDeclContext.h"
#include "clang/AST/ParentMap.h"
#include "llvm/Support/raw_ostream.h"
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

// Finds the LabelStmt bound to a target LabelDecl in a function body.
struct LabelCollectorVisitor : public RecursiveASTVisitor<LabelCollectorVisitor> {
  const LabelDecl *Target = nullptr;
  const LabelStmt *Found = nullptr;
  bool VisitLabelStmt(LabelStmt *LS) {
    if (Target && LS->getDecl() == Target)
      Found = LS;
    return true;
  }
};

// True when the call's directly named callee matches Name (API contract).
static bool calleeIs(const CallExpr *CE, StringRef Name) {
  if (!CE)
    return false;
  const FunctionDecl *FD = CE->getDirectCallee();
  if (!FD)
    return false;
  const IdentifierInfo *II = FD->getIdentifier();
  return II && II->getName() == Name;
}

// True for literal null forms (0, NULL, __null, nullptr literal).
static bool isZeroLiteral(const Expr *E) {
  if (!E)
    return false;
  E = E->IgnoreParenImpCasts();
  if (isa<GNUNullExpr>(E) || isa<CXXNullPtrLiteralExpr>(E))
    return true;
  if (const auto *IL = dyn_cast<IntegerLiteral>(E))
    return IL->getValue().isZero();
  return false;
}

class SAGenTestChecker
  : public Checker<
        check::BeginFunction,
        check::Bind,
        check::PostStmt<BinaryOperator>,
        check::PostCall,
        check::PreStmt<GotoStmt>,
        check::BranchCondition> {
   mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Resource leak in loop iteration (net_device)",
                       "Memory Management")) {}

  void checkBeginFunction(CheckerContext &C) const;
  void checkBind(SVal Loc, SVal Val, const Stmt *StoreE, CheckerContext &C) const;
  void checkPostStmt(const BinaryOperator *BO, CheckerContext &C) const;
  void checkPostCall(const CallEvent &Call, CheckerContext &C) const;
  void checkPreStmt(const GotoStmt *GS, CheckerContext &C) const;
  void checkBranchCondition(const Stmt *Condition, CheckerContext &C) const;

private:
  // Helpers
  static bool isAllocNetdevCall(const CallExpr *CE);
  static bool isExitLikeLabel(const LabelDecl *LD);
  static const Stmt* findEnclosingLoop(const Stmt *S, CheckerContext &C);
  static bool gotoEscapesLoop(const GotoStmt *GS, const Stmt *LoopS,
                              CheckerContext &C);
  static bool conditionMeansRegionNullOnThen(const Stmt *Condition,
                                             const MemRegion *R,
                                             CheckerContext &C);
  static const MemRegion* varRegionOfExpr(const Expr *E, CheckerContext &C);
  static bool stmtContains(const Stmt *Outer, const Stmt *Needle);

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
      if (!calleeIs(CE, "free_netdev"))
        continue;
      if (CE->getNumArgs() < 1)
        continue;
      const Expr *Arg0 = CE->getArg(0);
      if (!Arg0) continue;
      const MemRegion *ArgR = varRegionOfExpr(Arg0, C);
      if (!ArgR) continue;
      ArgR = ArgR->getBaseRegion();
      ArgR = canonicalizeRegion(State, ArgR);
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
  // Inlined callees share the caller's path state; resetting tracking there
  // would erase pending netdev ownership established before helper calls
  // (e.g. eth_hw_addr_random / rvu_rep_devlink_port_register between the
  // allocation and the failing branch). Reset only at the top frame.
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

  // Detect allocation assignment inside a loop: ptr = alloc_etherdev/alloc_netdev...
  if (StoreE && DstR) {
    CallCollectorVisitor CV;
    CV.TraverseStmt(const_cast<Stmt *>(StoreE));
    const CallExpr *AllocCE = nullptr;
    for (const CallExpr *CE : CV.Calls) {
      if (isAllocNetdevCall(CE)) {
        AllocCE = CE;
        break;
      }
    }
    if (AllocCE) {
      // Only allocations made inside a loop body create per-iteration ownership.
      const Stmt *LoopS = findEnclosingLoop(StoreE, C);
      if (LoopS) {
        State = State->set<PendingNetdevMap>(DstR, StoreE);
        State = State->set<NetdevLoopMap>(DstR, LoopS);
        Changed = true;
      }
    }
  }

  if (Changed)
    C.addTransition(State);
}

// Ownership-creating assignment evaluated as a completed statement:
// `ndev = alloc_etherdev/alloc_netdev*(...)` inside a loop body. Both the
// goto-escape and the guard-branch sinks consume PendingNetdevMap, so the
// pending per-iteration ownership must be registered here, where the LHS
// variable region and the RHS allocation call are directly available from
// the assignment expression itself.
void SAGenTestChecker::checkPostStmt(const BinaryOperator *BO,
                                     CheckerContext &C) const {
  if (!BO || !BO->isAssignmentOp())
    return;

  const Expr *RHS = BO->getRHS();
  if (!RHS)
    return;

  // The allocation may be wrapped in macros/casts; scan the RHS subtree for
  // a netdev allocator call (kernel macros expand to alloc_netdev_mqs).
  CallCollectorVisitor CV;
  CV.TraverseStmt(const_cast<Expr *>(RHS));
  const CallExpr *AllocCE = nullptr;
  for (const CallExpr *CE : CV.Calls) {
    if (isAllocNetdevCall(CE)) {
      AllocCE = CE;
      break;
    }
  }
  if (!AllocCE)
    return;

  const Expr *LHS = BO->getLHS();
  if (!LHS)
    return;
  const MemRegion *DstR = varRegionOfExpr(LHS, C);
  if (!DstR) {
    SVal LV = C.getSVal(LHS);
    if (std::optional<loc::MemRegionVal> MRV = LV.getAs<loc::MemRegionVal>())
      DstR = MRV->getRegion();
  }
  if (!DstR)
    return;
  DstR = DstR->getBaseRegion();

  // Per-iteration ownership exists only for allocations made inside a loop
  // body; the enclosing loop scopes the later escape/leak analysis.
  const Stmt *LoopS = findEnclosingLoop(BO, C);
  if (!LoopS)
    return;

  ProgramStateRef State = C.getState();
  State = State->set<PendingNetdevMap>(DstR, BO);
  State = State->set<NetdevLoopMap>(DstR, LoopS);
  C.addTransition(State);
}

void SAGenTestChecker::checkPostCall(const CallEvent &Call, CheckerContext &C) const {
  ProgramStateRef State = C.getState();
  const Expr *Origin = Call.getOriginExpr();
  if (!Origin)
    return;

  // If register_netdev(arg0) is called, the ownership is now transferred; stop tracking.
  if (calleeIs(dyn_cast<const CallExpr>(Origin), "register_netdev")) {
    if (Call.getNumArgs() >= 1) {
      const Expr *Arg0 = Call.getArgExpr(0);
      if (Arg0) {
        const MemRegion *ArgR = varRegionOfExpr(Arg0, C);
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
  if (calleeIs(dyn_cast<const CallExpr>(Origin), "free_netdev")) {
    if (Call.getNumArgs() >= 1) {
      const Expr *Arg0 = Call.getArgExpr(0);
      if (Arg0) {
        const MemRegion *ArgR = varRegionOfExpr(Arg0, C);
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

// Escape trigger at the goto statement itself. The ownership state is
// authoritative: free_netdev/register_netdev executed on this path already
// erased released regions, so a still-pending region at a goto that escapes
// the allocation's loop (or targets a cleanup label) leaks the current
// net_device allocated in that iteration.
void SAGenTestChecker::checkPreStmt(const GotoStmt *GS, CheckerContext &C) const {
  if (!GS)
    return;
  const LabelDecl *LD = GS->getLabel();
  if (!LD)
    return;

  // Ownership scope: the loop that created the pending allocation.
  const Stmt *LoopS = findEnclosingLoop(GS, C);

  // The goto must leave that scope: target label outside the enclosing loop
  // or a cleanup/exit-style label.
  if (LoopS) {
    if (!(isExitLikeLabel(LD) || gotoEscapesLoop(GS, LoopS, C)))
      return;
  } else if (!isExitLikeLabel(LD)) {
    return;
  }

  ProgramStateRef State = C.getState();
  auto Pend = State->get<PendingNetdevMap>();
  if (Pend.isEmpty())
    return;

  // Guard ifs whose then-branch contains this goto (e.g. `if (!ndev) goto`).
  SmallVector<const IfStmt *, 4> Guards;
  {
    ParentMap &PM =
        C.getLocationContext()->getAnalysisDeclContext()->getParentMap();
    for (const Stmt *P = PM.getParent(GS); P; P = PM.getParent(P)) {
      if (const auto *IS = dyn_cast<IfStmt>(P))
        if (stmtContains(IS->getThen(), GS))
          Guards.push_back(IS);
    }
  }

  for (auto I = Pend.begin(), E = Pend.end(); I != E; ++I) {
    const MemRegion *R = I->first;
    if (!R) continue;
    R = R->getBaseRegion();

    auto RLoop = State->get<NetdevLoopMap>(R);
    if (LoopS) {
      if (!RLoop || *RLoop != LoopS)
        continue;
    } else if (RLoop) {
      // A goto outside any loop only concerns loopless allocations.
      continue;
    }

    // Allocation-failure guard: the guard condition implies this pointer is
    // NULL on the taken path, so there is no live object to leak.
    bool NullGuarded = false;
    for (const IfStmt *G : Guards) {
      if (conditionMeansRegionNullOnThen(G->getCond(), R, C)) {
        NullGuarded = true;
        break;
      }
    }
    if (NullGuarded)
      continue;

    // AST fallback mirroring the ownership state: a guard branch that frees
    // this region means no leak even if the release left no state trace.
    bool GuardFrees = false;
    for (const IfStmt *G : Guards) {
      if (thenContainsFreeOfRegion(G->getThen(), State, R, C)) {
        GuardFrees = true;
        break;
      }
    }
    if (GuardFrees)
      continue;

    ExplodedNode *N = C.generateNonFatalErrorNode();
    if (!N)
      return;
    auto Rpt = std::make_unique<PathSensitiveBugReport>(
        *BT, "goto to exit label escapes loop iteration with unfreed net_device",
        N);
    Rpt->addRange(GS->getSourceRange());
    C.emitReport(std::move(Rpt));
  }
}

void SAGenTestChecker::checkBranchCondition(const Stmt *Condition, CheckerContext &C) const {
  if (!Condition)
    return;

  // Find the IfStmt that owns this condition by walking the CFG parent map.
  const IfStmt *IS = nullptr;
  {
    ParentMap &PM =
        C.getLocationContext()->getAnalysisDeclContext()->getParentMap();
    const Stmt *Cur = Condition;
    while (const Stmt *P = PM.getParent(Cur)) {
      if (const auto *OwnerIf = dyn_cast<IfStmt>(P)) {
        IS = OwnerIf;
        break;
      }
      // Stop at control boundaries: the condition does not belong to an if.
      if (isa<CompoundStmt>(P) || isa<WhileStmt>(P) || isa<ForStmt>(P) ||
          isa<DoStmt>(P) || isa<SwitchStmt>(P))
        break;
      Cur = P;
    }
  }
  if (!IS)
    return;

  const Stmt *Then = IS->getThen();
  if (!Then)
    return;

  // Find enclosing loop of this if-statement first: the escape target must
  // leave the loop that owns the pending allocation.
  const Stmt *LoopS = findEnclosingLoop(IS, C);
  if (!LoopS)
    return;

  // Does the then-branch contain a goto that escapes that loop, either to an
  // exit-like label or to a label located outside the loop statement subtree?
  GotoCollectorVisitor GV;
  GV.TraverseStmt(const_cast<Stmt *>(Then));

  bool HasExitLikeGoto = false;
  for (const GotoStmt *GS : GV.Gotos) {
    if (!GS) continue;
    const LabelDecl *LD = GS->getLabel();
    if (isExitLikeLabel(LD) || gotoEscapesLoop(GS, LoopS, C)) {
      HasExitLikeGoto = true;
      break;
    }
  }
  if (!HasExitLikeGoto)
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

    // If the branch condition implies this pointer is NULL on the taken
    // path, the allocation itself failed and there is no live object to
    // leak (e.g. `if (!ndev) goto exit;` right after the allocator).
    if (conditionMeansRegionNullOnThen(Condition, R, C))
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

bool SAGenTestChecker::stmtContains(const Stmt *Outer, const Stmt *Needle) {
  if (!Outer || !Needle)
    return false;
  if (Outer == Needle)
    return true;
  for (const Stmt *Ch : Outer->children())
    if (stmtContains(Ch, Needle))
      return true;
  return false;
}

bool SAGenTestChecker::isAllocNetdevCall(const CallExpr *CE) {
  // Recognize common Linux netdev allocation helpers via the direct callee.
  return calleeIs(CE, "alloc_etherdev") ||
         calleeIs(CE, "alloc_etherdev_mqs") ||
         calleeIs(CE, "alloc_netdev") ||
         calleeIs(CE, "alloc_netdev_mqs");
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
  if (LRef.starts_with("err") || LRef.starts_with("error") ||
      LRef.starts_with("out"))
    return true;

  return false;
}

const MemRegion* SAGenTestChecker::varRegionOfExpr(const Expr *E,
                                                   CheckerContext &C) {
  if (!E) return nullptr;
  const Expr *UE = E->IgnoreParenImpCasts();

  // Resolve through the store so calls inspected inside an AST subtree
  // (not yet evaluated on this path) still map to the same variable region.
  if (const auto *DRE = dyn_cast<DeclRefExpr>(UE)) {
    if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
      SVal LV = C.getState()->getLValue(VD, C.getLocationContext());
      if (std::optional<loc::MemRegionVal> MRV =
              LV.getAs<loc::MemRegionVal>())
        return MRV->getRegion();
    }
  }

  SVal V = C.getSVal(UE);
  if (std::optional<loc::MemRegionVal> MRV = V.getAs<loc::MemRegionVal>())
    return MRV->getRegion();
  return nullptr;
}

const Stmt* SAGenTestChecker::findEnclosingLoop(const Stmt *S, CheckerContext &C) {
  if (!S) return nullptr;
  AnalysisDeclContext *ADC = C.getLocationContext()->getAnalysisDeclContext();
  if (!ADC) return nullptr;
  ParentMap &PM = ADC->getParentMap();
  for (const Stmt *P = PM.getParent(S); P; P = PM.getParent(P)) {
    if (isa<ForStmt>(P) || isa<WhileStmt>(P) || isa<DoStmt>(P))
      return P;
  }
  return nullptr;
}

bool SAGenTestChecker::gotoEscapesLoop(const GotoStmt *GS, const Stmt *LoopS,
                                       CheckerContext &C) {
  if (!GS || !LoopS)
    return false;
  const LabelDecl *LD = GS->getLabel();
  if (!LD)
    return false;

  AnalysisDeclContext *ADC = C.getLocationContext()->getAnalysisDeclContext();
  if (!ADC)
    return false;
  const Stmt *Body = ADC->getBody();
  if (!Body)
    return false;

  LabelCollectorVisitor LCV;
  LCV.Target = LD;
  LCV.TraverseStmt(const_cast<Stmt *>(Body));
  if (!LCV.Found)
    return false;

  // The goto escapes the loop when its target label lies outside the loop's
  // statement subtree (the loop's cleanup cannot reclaim this iteration).
  ParentMap &PM = ADC->getParentMap();
  for (const Stmt *P = PM.getParent(LCV.Found); P; P = PM.getParent(P)) {
    if (P == LoopS)
      return false; // label inside the loop: not an escape
  }
  return true;
}

bool SAGenTestChecker::conditionMeansRegionNullOnThen(const Stmt *Condition,
                                                      const MemRegion *R,
                                                      CheckerContext &C) {
  if (!Condition || !R)
    return false;
  const auto *CondE = dyn_cast<Expr>(Condition);
  if (!CondE)
    return false;
  CondE = CondE->IgnoreParenImpCasts();

  // `!p` where p denotes R: taking the then-branch implies p == NULL.
  if (const auto *UO = dyn_cast<UnaryOperator>(CondE)) {
    if (UO->getOpcode() == UO_LNot) {
      const MemRegion *SubR = varRegionOfExpr(UO->getSubExpr(), C);
      return SubR && SubR->getBaseRegion() == R;
    }
    return false;
  }

  // `p == NULL` or `NULL == p`.
  if (const auto *BO = dyn_cast<BinaryOperator>(CondE)) {
    if (BO->getOpcode() == BO_EQ) {
      const Expr *L = BO->getLHS()->IgnoreParenImpCasts();
      const Expr *RHS = BO->getRHS()->IgnoreParenImpCasts();
      const Expr *PtrSide = nullptr;
      if (isZeroLiteral(L))
        PtrSide = RHS;
      else if (isZeroLiteral(RHS))
        PtrSide = L;
      if (!PtrSide)
        return false;
      const MemRegion *PtrR = varRegionOfExpr(PtrSide, C);
      return PtrR && PtrR->getBaseRegion() == R;
    }
  }
  return false;
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
