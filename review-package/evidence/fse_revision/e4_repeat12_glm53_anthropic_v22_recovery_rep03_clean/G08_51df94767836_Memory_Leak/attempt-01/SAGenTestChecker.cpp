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
// Tracks the allocation symbol of a net_device this function still owns for
// the current loop iteration (allocated, not yet freed or registered).
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
        check::PostCall,
        check::BranchCondition> {
   mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Resource leak in loop iteration (net_device)",
                       "Memory Management")) {}

  void checkBeginFunction(CheckerContext &C) const;
  void checkPostCall(const CallEvent &Call, CheckerContext &C) const;
  void checkBranchCondition(const Stmt *Condition, CheckerContext &C) const;

private:
  // Helpers
  static bool isAllocNetdevCall(const Expr *E, CheckerContext &C);
  static bool isExitLikeLabel(const LabelDecl *LD);
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
      if (!ExprHasName(CE, "free_netdev", C))
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
  if (ExprHasName(Origin, "register_netdev", C) ||
      ExprHasName(Origin, "free_netdev", C)) {
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
