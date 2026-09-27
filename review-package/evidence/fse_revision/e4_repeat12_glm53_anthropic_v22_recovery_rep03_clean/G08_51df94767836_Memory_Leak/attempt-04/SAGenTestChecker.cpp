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
#include "clang/StaticAnalyzer/Core/PathSensitive/ConstraintManager.h"
#include "llvm/ADT/APSInt.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtVisitor.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ParentMapContext.h"
#include "llvm/ADT/StringExtras.h"

using namespace clang;
using namespace ento;
using namespace taint;

// Program state maps
// Tracks the allocation symbol of a net_device this function still owns for
// the current loop iteration (allocated, not yet freed or registered).
REGISTER_MAP_WITH_PROGRAMSTATE(PendingNetdevMap, SymbolRef, const Stmt*)
// Tracks the loop statement where the allocation happened (For/While/Do).
REGISTER_MAP_WITH_PROGRAMSTATE(NetdevLoopMap, SymbolRef, const Stmt*)

namespace {

// Resolves the callee FunctionDecl at a call site through the AST itself, so
// acquisition/release binding does not depend on helper-side name matching.
static const FunctionDecl *getCalleeFnDecl(const Expr *E) {
  if (!E)
    return nullptr;
  const auto *CE = dyn_cast<CallExpr>(E->IgnoreParenImpCasts());
  if (!CE)
    return nullptr;
  return dyn_cast_or_null<FunctionDecl>(CE->getCalleeDecl());
}

static bool callHasCalleeName(const Expr *E, StringRef Name) {
  const FunctionDecl *FD = getCalleeFnDecl(E);
  return FD && FD->getName() == Name;
}

// Walks AST parents upward and returns the closest ancestor of type T.
template <typename T>
static const T *findAncestorOfType(const Stmt *S, CheckerContext &C) {
  if (!S)
    return nullptr;
  ASTContext &AC = C.getASTContext();
  const Stmt *Cur = S;
  for (unsigned Depth = 0; Depth < 128; ++Depth) {
    DynTypedNodeList Parents = AC.getParentMapContext().getParents(*Cur);
    if (Parents.empty())
      return nullptr;
    const Stmt *Next = nullptr;
    for (const auto &P : Parents) {
      if (const T *Typed = P.template get<T>())
        return Typed;
      if (!Next)
        if (const Stmt *SN = P.get<Stmt>())
          Next = SN;
    }
    if (!Next || Next == Cur)
      return nullptr;
    Cur = Next;
  }
  return nullptr;
}

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

// True if the goto jumps to a label that lies outside the enclosing loop:
// control leaves the current iteration while the tracked allocation is
// still owned by it. Structural, so a consistent label rename keeps the
// trigger.
static bool gotoEscapesEnclosingLoop(const GotoStmt *GS, const Stmt *LoopS,
                                     CheckerContext &C) {
  if (!GS || !LoopS)
    return false;
  const LabelDecl *LD = GS->getLabel();
  if (!LD)
    return false;

  // Resolve the target label statement inside the analyzed function body.
  const auto *FD =
      dyn_cast_or_null<FunctionDecl>(C.getLocationContext()->getDecl());
  if (!FD)
    return false;
  Stmt *Body = FD->getBody();
  if (!Body)
    return false;
  LabelCollectorVisitor LV;
  LV.Target = LD;
  LV.TraverseStmt(Body);
  if (!LV.Found)
    return false;

  // The jump escapes the loop when the loop statement is not an ancestor
  // of the label statement.
  ASTContext &AC = C.getASTContext();
  const Stmt *Cur = LV.Found;
  for (unsigned Depth = 0; Depth < 128; ++Depth) {
    if (Cur == LoopS)
      return false;
    DynTypedNodeList Parents = AC.getParentMapContext().getParents(*Cur);
    if (Parents.empty())
      break;
    const Stmt *Next = nullptr;
    for (const auto &P : Parents) {
      if (!Next)
        if (const Stmt *SN = P.get<Stmt>())
          Next = SN;
    }
    if (!Next || Next == Cur)
      break;
    Cur = Next;
  }
  return true;
}

class SAGenTestChecker
  : public Checker<
        check::BeginFunction,
        check::PostCall,
        check::BranchCondition,
           check::PreStmt<GotoStmt>> {
   mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Resource leak in loop iteration (net_device)",
                       "Memory Management")) {}

  void checkBeginFunction(CheckerContext &C) const;
  void checkPostCall(const CallEvent &Call, CheckerContext &C) const;
  void checkBranchCondition(const Stmt *Condition, CheckerContext &C) const;
  void checkPreStmt(const GotoStmt *GS, CheckerContext &C) const;

private:
  // Helpers
  static bool isAllocNetdevCall(const Expr *E, CheckerContext &C);
  static const Stmt* findEnclosingLoop(const Stmt *S, CheckerContext &C);

  // True if some free_netdev call in Then releases exactly this allocation
  // symbol. The freed pointer is evaluated on the state observed at the
  // branch, where the owning variable still holds the allocation symbol.
  static bool thenContainsFreeOfSym(const Stmt *Then, ProgramStateRef State,
                                    SymbolRef TargetSym, CheckerContext &C) {
    if (!Then || !TargetSym)
      return false;

    CallCollectorVisitor V;
    V.TraverseStmt(const_cast<Stmt *>(Then));
    for (const CallExpr *CE : V.Calls) {
      if (!CE) continue;
      if (!callHasCalleeName(CE, "free_netdev"))
        continue;
      if (CE->getNumArgs() < 1)
        continue;
      const Expr *Arg0 = CE->getArg(0);
      if (!Arg0) continue;
      SVal ArgV = State->getSVal(Arg0, C.getLocationContext());
      if (ArgV.getAsSymbol(/*IncludeBaseRegions=*/true) == TargetSym)
        return true;
    }
    return false;
  }

  // True when the then-arm of this if is only reachable while the tracked
  // allocation symbol is NULL: the allocation itself failed, no object
  // exists, and nothing can leak on that arm.
  static bool thenBranchIsNullPath(const Stmt *Condition, SymbolRef Sym,
                                   CheckerContext &C) {
    if (!Condition || !Sym)
      return false;
    const auto *E = dyn_cast<Expr>(Condition);
    if (!E)
      return false;
    E = E->IgnoreParenImpCasts();

    const Expr *Operand = nullptr;
    bool TrueMeansNull = false;

    if (const auto *UO = dyn_cast<UnaryOperator>(E)) {
      if (UO->getOpcode() == UO_LNot) {
        Operand = UO->getSubExpr()->IgnoreParenImpCasts();
        TrueMeansNull = true;
      }
    } else if (const auto *BO = dyn_cast<BinaryOperator>(E)) {
      if (BO->isEqualityOp()) {
        ProgramStateRef St = C.getState();
        SVal LV = St->getSVal(BO->getLHS(), C.getLocationContext());
        SVal RV = St->getSVal(BO->getRHS(), C.getLocationContext());
        bool IsEq = BO->getOpcode() == BO_EQ;
        if (LV.isZeroConstant()) {
          Operand = BO->getRHS()->IgnoreParenImpCasts();
          TrueMeansNull = IsEq;
        } else if (RV.isZeroConstant()) {
          Operand = BO->getLHS()->IgnoreParenImpCasts();
          TrueMeansNull = IsEq;
        }
      }
    }
    if (!Operand || !TrueMeansNull)
      return false;

    SVal OpV = C.getState()->getSVal(Operand, C.getLocationContext());
    return OpV.getAsSymbol(/*IncludeBaseRegions=*/true) == Sym;
  }

  static void erasePendingFor(ProgramStateRef &State, SymbolRef Sym) {
    if (!Sym) return;
    State = State->remove<PendingNetdevMap>(Sym);
    State = State->remove<NetdevLoopMap>(Sym);
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

  C.addTransition(State);
}

// Pointer ownership is bound to the allocation's return symbol in
// checkPostCall; symbol identity in the store already follows every
// assignment of the owning pointer variable, so no separate alias map is
// needed.

void SAGenTestChecker::checkPostCall(const CallEvent &Call, CheckerContext &C) const {
  ProgramStateRef State = C.getState();
  const Expr *Origin = Call.getOriginExpr();
  if (!Origin)
    return;

  // Allocation site: an alloc_netdev-family call inside a loop starts
  // ownership of the returned net_device. An unmodeled allocator returns a
  // conjured pointer symbol rather than a materialized heap region, so
  // ownership is keyed by that symbol; symbol identity also survives every
  // store and reload of the owning pointer variable.
  if (isAllocNetdevCall(Origin, C)) {
    if (SymbolRef Sym =
            Call.getReturnValue().getAsSymbol(/*IncludeBaseRegions=*/true)) {
      if (const Stmt *LoopS = findEnclosingLoop(Origin, C)) {
        ProgramStateRef NewState = State->set<PendingNetdevMap>(Sym, Origin);
        NewState = NewState->set<NetdevLoopMap>(Sym, LoopS);
        C.addTransition(NewState);
      }
    }
    return;
  }

  // register_netdev(arg0) transfers ownership to the networking stack and
  // free_netdev(arg0) releases it; both end this function's obligation for
  // the allocation symbol they are called with.
  if (callHasCalleeName(Origin, "register_netdev") ||
      callHasCalleeName(Origin, "free_netdev")) {
    if (Call.getNumArgs() >= 1) {
      if (SymbolRef Sym =
              Call.getArgSVal(0).getAsSymbol(/*IncludeBaseRegions=*/true)) {
        if (State->get<NetdevLoopMap>(Sym) ||
            State->get<PendingNetdevMap>(Sym)) {
          erasePendingFor(State, Sym);
          C.addTransition(State);
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
  const IfStmt *IS = findAncestorOfType<IfStmt>(Condition, C);
  if (!IS)
    return;

  const Stmt *Then = IS->getThen();
  if (!Then)
    return;

  // Find enclosing loop of this if-statement.
  const Stmt *LoopS = findEnclosingLoop(IS, C);
  if (!LoopS)
    return;

  // Does the then-branch leave the loop (jump to a label outside it) while
  // the current iteration still owns the allocation?
  GotoCollectorVisitor GV;
  GV.TraverseStmt(const_cast<Stmt *>(Then));

  bool HasLoopEscapingGoto = false;
  for (const GotoStmt *GS : GV.Gotos) {
    if (!GS) continue;
    if (gotoEscapesEnclosingLoop(GS, LoopS, C)) {
      HasLoopEscapingGoto = true;
      break;
    }
  }
  if (!HasLoopEscapingGoto)
    return;

  ProgramStateRef State = C.getState();
  auto Pend = State->get<PendingNetdevMap>();
  if (Pend.isEmpty())
    return;

  // For each net_device owned for this loop, the exit branch must release it.
  for (auto I = Pend.begin(), E = Pend.end(); I != E; ++I) {
    SymbolRef Sym = I->first;
    if (!Sym) continue;

    auto RLoop = State->get<NetdevLoopMap>(Sym);
    if (!RLoop || *RLoop != LoopS)
      continue;

    // A branch only reachable when the allocation itself returned NULL
    // releases nothing because no object exists; not a leak path.
    if (thenBranchIsNullPath(Condition, Sym, C))
      continue;

    // Check whether Then frees the current iteration's net_device.
    if (!thenContainsFreeOfSym(Then, State, Sym, C)) {
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

// Escaping-jump sink. Ownership lives in the program state: an
// alloc_netdev-family call inside a loop marks the returned symbol
// pending for that loop, and free_netdev/register_netdev called with
// that same symbol clear it. When a goto is about to execute while its
// enclosing loop still owns such a pending allocation and the target
// label lies outside that loop, control abandons the current iteration
// without releasing it, so the leak is reported at the jump itself.
void SAGenTestChecker::checkPreStmt(const GotoStmt *GS, CheckerContext &C) const {
  if (!GS)
    return;

  const Stmt *LoopS = findEnclosingLoop(GS, C);
  if (!LoopS)
    return;

  // Only jumps whose target label lies outside the enclosing loop drop
  // the current iteration's ownership obligation.
  if (!gotoEscapesEnclosingLoop(GS, LoopS, C))
    return;

  ProgramStateRef State = C.getState();
  auto Pend = State->get<PendingNetdevMap>();
  if (Pend.isEmpty())
    return;

  for (auto I = Pend.begin(), E = Pend.end(); I != E; ++I) {
    SymbolRef Sym = I->first;
    if (!Sym) continue;

    auto RLoop = State->get<NetdevLoopMap>(Sym);
    if (!RLoop || *RLoop != LoopS)
      continue;

    // Silence condition (shape): the jump leaves from an arm reachable
    // only when this allocation itself returned NULL; no object exists,
    // nothing can leak.
    bool NullArmExit = false;
    if (const IfStmt *NullIS = findAncestorOfType<IfStmt>(GS, C)) {
      const Stmt *NullThen = NullIS->getThen();
      if (NullThen) {
        GotoCollectorVisitor TV;
        TV.TraverseStmt(const_cast<Stmt *>(NullThen));
        for (const GotoStmt *TG : TV.Gotos) {
          if (TG == GS && thenBranchIsNullPath(NullIS->getCond(), Sym, C)) {
            NullArmExit = true;
            break;
          }
        }
      }
    }

    // Silence condition (path state): the symbol is concretely NULL on
    // this path, i.e. the allocation failed and was never owned.
    bool ConcreteNull = false;
    if (const llvm::APSInt *Concrete =
            C.getConstraintManager().getSymVal(State, Sym))
      ConcreteNull = Concrete->isZero();

    if (NullArmExit || ConcreteNull)
      continue;

    ExplodedNode *N = C.generateNonFatalErrorNode();
    if (!N)
      return;

    auto Rpt = std::make_unique<PathSensitiveBugReport>(
        *BT, "net_device allocated for this loop iteration is still "
             "owned when goto leaves the loop",
        N);
    Rpt->addRange(GS->getSourceRange());
    C.emitReport(std::move(Rpt));
  }
}

// Helper implementations

bool SAGenTestChecker::isAllocNetdevCall(const Expr *E, CheckerContext &C) {
  (void)C;
  if (!E) return false;
  // Acquisition side of the net_device ownership contract: a direct call
  // into the alloc_etherdev/alloc_netdev family returns a fresh net_device
  // that this caller must eventually register or free. The callee is
  // resolved through the FunctionDecl at the call site, so matching happens
  // only at the kernel API boundary, never at analyzed-driver names.
  const FunctionDecl *FD = getCalleeFnDecl(E);
  if (!FD) return false;
  StringRef Name = FD->getName();
  return Name == "alloc_etherdev" || Name == "alloc_netdev" ||
         Name.starts_with("alloc_etherdev_") ||
         Name.starts_with("alloc_netdev_");
}

const Stmt* SAGenTestChecker::findEnclosingLoop(const Stmt *S, CheckerContext &C) {
  if (!S) return nullptr;
  if (const auto *FS = findAncestorOfType<ForStmt>(S, C))
    return FS;
  if (const auto *WS = findAncestorOfType<WhileStmt>(S, C))
    return WS;
  if (const auto *DS = findAncestorOfType<DoStmt>(S, C))
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
