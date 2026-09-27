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
#include "clang/AST/ParentMapContext.h"
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
// The ownership identity of an allocated net_device is the pointer value the
// allocation returned (its symbol): every variable holding that pointer reads
// the same symbol, so ownership survives local copies without an alias table.
// Tracks an allocated net_device still owned by the current loop iteration.
REGISTER_MAP_WITH_PROGRAMSTATE(PendingNetdevMap, SymbolRef, const Stmt*)
// Tracks the loop statement where the allocation happened (For/While/Do).
REGISTER_MAP_WITH_PROGRAMSTATE(NetdevLoopMap, SymbolRef, const Stmt*)

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
  // First netdev allocation call in S's subtree (S itself included), so the
  // store wrapper (assignment vs declaration initializer) cannot hide it.
  static const CallExpr *findAllocCallInStmt(const Stmt *S, CheckerContext &C);

  // Match a call by its callee symbol (the actual acquisition/release API),
  // independent of the full call-expression spelling with arguments.
  static bool calleeHasName(const Expr *Origin, StringRef Name,
                            CheckerContext &C) {
    const auto *CE = dyn_cast_or_null<CallExpr>(Origin);
    if (!CE)
      return false;
    if (const FunctionDecl *FD = CE->getDirectCallee())
      return FD->getName() == Name;
    const Expr *CalleeE = CE->getCallee();
    return CalleeE && ExprHasName(CalleeE, Name, C);
  }

  // Resolve an expression to the tracked allocated-object symbol: a pointer
  // rvalue carrying the allocation result is the object itself.
  static SymbolRef resolveTrackedSymbol(const Expr *E, CheckerContext &C) {
    if (!E)
      return nullptr;
    SVal V = C.getSVal(E);
    SymbolRef S = V.getAsSymbol();
    if (!S)
      S = V.getAsLocSymbol();
    return S;
  }

  // True when the then-branch of Condition is entered only while the tracked
  // allocation pointer is null (an allocation-failure guard such as
  // `if (!ptr)`): on that path the object was never produced, so there is
  // nothing to release.
  static bool thenBranchGuardsNull(const Stmt *Condition, SymbolRef S,
                                   CheckerContext &C) {
    if (!Condition || !S)
      return false;
    const auto *CondE = dyn_cast<Expr>(Condition);
    if (!CondE)
      return false;
    const auto *UO = dyn_cast<UnaryOperator>(CondE->IgnoreParenImpCasts());
    if (!UO || UO->getOpcode() != UO_LNot)
      return false;
    SymbolRef Tested = resolveTrackedSymbol(UO->getSubExpr(), C);
    return Tested && Tested == S;
  }

  // True when Then releases the tracked allocated object before leaving
  // (free_netdev called with the same pointer value the allocation produced).
  static bool thenContainsFreeOfSymbol(const Stmt *Then, CheckerContext &C,
                                       SymbolRef TargetS) {
    if (!Then || !TargetS)
      return false;

    CallCollectorVisitor V;
    V.TraverseStmt(const_cast<Stmt *>(Then));
    for (const CallExpr *CE : V.Calls) {
      if (!CE) continue;
      if (!calleeHasName(CE, "free_netdev", C))
        continue;
      if (CE->getNumArgs() < 1)
        continue;
      const Expr *Arg0 = CE->getArg(0);
      if (!Arg0) continue;
      SymbolRef ArgS = resolveTrackedSymbol(Arg0, C);
      if (ArgS && ArgS == TargetS)
        return true;
    }
    return false;
  }

  // Ownership of the object left this function's tracking (transferred to the
  // network stack or released); later error arms must not report it.
  void erasePendingFor(ProgramStateRef &State, SymbolRef S) const {
    if (!S) return;
    State = State->remove<PendingNetdevMap>(S);
    State = State->remove<NetdevLoopMap>(S);
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
  auto LM = State->get<NetdevLoopMap>();
  if (!LM.isEmpty()) {
    for (auto I = LM.begin(), E = LM.end(); I != E; ++I) {
      State = State->remove<NetdevLoopMap>(I->first);
    }
  }
  C.addTransition(State);
}

void SAGenTestChecker::checkBind(
                                 CheckerContext &C) const {
  ProgramStateRef State = C.getState();
  bool Changed = false;

  // Detect allocation assignment inside a loop: ptr = alloc_etherdev/alloc_netdev...
  // The stored pointer value is the owned object; loop membership of the
  // acquisition makes the object releasable by this iteration's error arms.
  if (StoreE) {
    const CallExpr *CE = findAllocCallInStmt(StoreE, C);
    if (CE) {
      SymbolRef AllocSym = Val.getAsSymbol();
      if (!AllocSym)
        AllocSym = Val.getAsLocSymbol();
      if (AllocSym) {
        // Find nearest enclosing loop
        const Stmt *LoopS = findEnclosingLoop(StoreE, C);
        if (LoopS) {
          State = State->set<PendingNetdevMap>(AllocSym, StoreE);
          State = State->set<NetdevLoopMap>(AllocSym, LoopS);
          Changed = true;
        }
      }
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

  // register_netdev transfers ownership to the network stack; free_netdev
  // releases it. Either way this function no longer owns the object.
  if (!calleeHasName(Origin, "register_netdev", C) &&
      !calleeHasName(Origin, "free_netdev", C))
    return;

  if (Call.getNumArgs() < 1)
    return;
  const Expr *Arg0 = Call.getArgExpr(0);
  if (!Arg0)
    return;
  SymbolRef ArgS = resolveTrackedSymbol(Arg0, C);
  if (!ArgS)
    return;
  auto LoopPtr = State->get<NetdevLoopMap>(ArgS);
  auto AllocPtr = State->get<PendingNetdevMap>(ArgS);
  if (LoopPtr || AllocPtr) {
    erasePendingFor(State, ArgS);
    C.addTransition(State);
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
    SymbolRef S = I->first;
    if (!S) continue;

    auto RLoop = State->get<NetdevLoopMap>(S);
    if (!RLoop || *RLoop != LoopS)
      continue;

    // An allocation-failure guard (`if (!ndev)`) reaches Then only with the
    // allocation pointer constrained to null: the object was never produced,
    // so no leak is possible on this arm.
    if (thenBranchGuardsNull(Condition, S, C))
      continue;

    // Check whether Then frees the current iteration's net_device.
    if (!thenContainsFreeOfSymbol(Then, C, S)) {
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
  // Recognize common Linux netdev allocation helpers by callee symbol.
  return calleeHasName(E, "alloc_etherdev", C) ||
         calleeHasName(E, "alloc_etherdev_mqs", C) ||
         calleeHasName(E, "alloc_netdev", C) ||
         calleeHasName(E, "alloc_netdev_mqs", C);
}

// First netdev allocation call found in S's subtree (S itself included).
const CallExpr *SAGenTestChecker::findAllocCallInStmt(const Stmt *S,
                                                      CheckerContext &C) {
  if (!S)
    return nullptr;
  CallCollectorVisitor V;
  V.TraverseStmt(const_cast<Stmt *>(S));
  for (const CallExpr *CE : V.Calls) {
    if (CE && isAllocNetdevCall(CE, C))
      return CE;
  }
  return nullptr;
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

// Climb the full ancestor chain of S (hopping through Decl owners such as
// VarDecl initializers) and return the innermost enclosing For/While/Do
// statement. A single-level parent lookup cannot see the loop across the
// CompoundStmt holding the loop body, so the climb must be explicit.
const Stmt* SAGenTestChecker::findEnclosingLoop(const Stmt *S, CheckerContext &C) {
  if (!S)
    return nullptr;
  ASTContext &Ctx = C.getASTContext();
  const Stmt *Cur = S;
  while (Cur) {
    DynTypedNodeList Parents = Ctx.getParents(*Cur);
    if (Parents.empty())
      break;
    const Stmt *NextStmt = nullptr;
    const Decl *Owner = nullptr;
    for (const DynTypedNode &P : Parents) {
      if (const auto *PS = P.get<Stmt>()) {
        if (isa<ForStmt>(PS) || isa<WhileStmt>(PS) || isa<DoStmt>(PS))
          return PS;
        if (!NextStmt)
          NextStmt = PS;
      } else if (!Owner) {
        Owner = P.get<Decl>();
      }
    }
    if (!NextStmt && Owner) {
      // The node is owned by a Decl (e.g. the initializer of
      // `struct net_device *nd = alloc_netdev(...)` under a VarDecl);
      // continue through the declaration's own parents.
      for (const DynTypedNode &P : Ctx.getParents(*Owner)) {
        if (const auto *PS = P.get<Stmt>()) {
          if (isa<ForStmt>(PS) || isa<WhileStmt>(PS) || isa<DoStmt>(PS))
            return PS;
          NextStmt = PS;
          break;
        }
      }
    }
    Cur = NextStmt;
  }
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
