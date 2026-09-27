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

// Tracks the pointer region that owns an allocated net_device in the
// current loop iteration.
REGISTER_MAP_WITH_PROGRAMSTATE(PendingNetdevMap, const MemRegion *, const Stmt *)
// Tracks the loop statement where the allocation happened.
REGISTER_MAP_WITH_PROGRAMSTATE(NetdevLoopMap, const MemRegion *, const Stmt *)
// Pointer aliasing: destination region -> source region.
REGISTER_MAP_WITH_PROGRAMSTATE(PtrAliasMap, const MemRegion *, const MemRegion *)

namespace {

// Collect calls in a statement subtree.
struct CallCollectorVisitor
    : public RecursiveASTVisitor<CallCollectorVisitor> {
  SmallVector<const CallExpr *, 16> Calls;

  bool VisitCallExpr(CallExpr *CE) {
    Calls.push_back(CE);
    return true;
  }
};

// Collect gotos in a statement subtree.
struct GotoCollectorVisitor
    : public RecursiveASTVisitor<GotoCollectorVisitor> {
  SmallVector<const GotoStmt *, 8> Gotos;

  bool VisitGotoStmt(GotoStmt *GS) {
    Gotos.push_back(GS);
    return true;
  }
};

class SAGenTestChecker
    : public Checker<check::BeginFunction, check::Bind, check::PostCall,
                     check::BranchCondition> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Resource leak in loop iteration (net_device)",
                       "Memory Management")) {}

  void checkBeginFunction(CheckerContext &C) const;
  void checkBind(SVal Loc, SVal Val, const Stmt *StoreE,
                 CheckerContext &C) const;
  void checkPostCall(const CallEvent &Call, CheckerContext &C) const;
  void checkBranchCondition(const Stmt *Condition, CheckerContext &C) const;

private:
  static bool isAllocNetdevCall(const Expr *E, CheckerContext &C);
  static bool isExitLikeLabel(const LabelDecl *LD);
  static const Stmt *findEnclosingLoop(const Stmt *S, CheckerContext &C);

  static const MemRegion *canonicalizeRegion(ProgramStateRef State,
                                              const MemRegion *R) {
    if (!R)
      return nullptr;

    R = R->getBaseRegion();
    for (int I = 0; I < 8 && R; ++I) {
      if (const MemRegion *const *R2 = State->get<PtrAliasMap>(R))
        R = (*R2)->getBaseRegion();
      else
        break;
    }
    return R;
  }

  static bool thenContainsFreeOfRegion(const Stmt *Then,
                                       ProgramStateRef State,
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
      if (!CE || !ExprHasName(CE, "free_netdev", C) ||
          CE->getNumArgs() < 1)
        continue;

      const Expr *Arg0 = CE->getArg(0);
      if (!Arg0)
        continue;

      const MemRegion *ArgR = getMemRegionFromExpr(Arg0, C);
      if (!ArgR)
        continue;

      ArgR = canonicalizeRegion(State, ArgR->getBaseRegion());
      if (ArgR && ArgR == CanonTarget)
        return true;
    }

    return false;
  }

  static bool isNullPointerConstantExpr(const Expr *E,
                                        CheckerContext &C) {
    if (!E)
      return false;

    E = E->IgnoreParenImpCasts();

    if (isa<CXXNullPtrLiteralExpr>(E) || isa<GNUNullExpr>(E))
      return true;

    // Handle common forms such as NULL expanding to a cast of integer zero.
    if (const auto *Cast = dyn_cast<CastExpr>(E))
      return isNullPointerConstantExpr(Cast->getSubExpr(), C);

    llvm::APSInt Value;
    return EvaluateExprToInt(Value, E, C) && Value == 0;
  }

  static bool expressionRefersToRegion(const Expr *E,
                                       const MemRegion *TargetR,
                                       ProgramStateRef State,
                                       CheckerContext &C) {
    if (!E || !TargetR)
      return false;

    const MemRegion *ExprR = getMemRegionFromExpr(E, C);
    if (!ExprR)
      return false;

    ExprR = canonicalizeRegion(State, ExprR);
    const MemRegion *CanonTarget = canonicalizeRegion(State, TargetR);
    return ExprR && CanonTarget && ExprR == CanonTarget;
  }

  // Returns true only when the if-condition tests TargetR for null and the
  // then-branch is the null case. For example, !ndev and ndev == NULL.
  static bool thenBranchTestsRegionForNull(const Expr *Condition,
                                            const MemRegion *TargetR,
                                            ProgramStateRef State,
                                            CheckerContext &C) {
    if (!Condition || !TargetR)
      return false;

    Condition = Condition->IgnoreParenImpCasts();

    if (const auto *UO = dyn_cast<UnaryOperator>(Condition)) {
      return UO->getOpcode() == UO_LNot &&
             expressionRefersToRegion(UO->getSubExpr(), TargetR, State, C);
    }

    const auto *BO = dyn_cast<BinaryOperator>(Condition);
    if (!BO || (BO->getOpcode() != BO_EQ && BO->getOpcode() != BO_NE))
      return false;

    const Expr *PointerExpr = nullptr;
    const Expr *OtherExpr = nullptr;

    if (expressionRefersToRegion(BO->getLHS(), TargetR, State, C)) {
      PointerExpr = BO->getLHS();
      OtherExpr = BO->getRHS();
    } else if (expressionRefersToRegion(BO->getRHS(), TargetR, State, C)) {
      PointerExpr = BO->getRHS();
      OtherExpr = BO->getLHS();
    } else {
      return false;
    }

    (void)PointerExpr;
    if (!isNullPointerConstantExpr(OtherExpr, C))
      return false;

    // `ptr == NULL` enters the null branch; `ptr != NULL` does not.
    return BO->getOpcode() == BO_EQ;
  }

  void erasePendingFor(ProgramStateRef &State, const MemRegion *R) const {
    if (!R)
      return;

    R = R->getBaseRegion();
    State = State->remove<PendingNetdevMap>(R);
    State = State->remove<NetdevLoopMap>(R);

    auto AM = State->get<PtrAliasMap>();
    if (!AM.isEmpty()) {
      for (auto It = AM.begin(), E = AM.end(); It != E; ++It) {
        if (It->first == R)
          State = State->remove<PtrAliasMap>(It->first);
      }
    }
  }
};

void SAGenTestChecker::checkBeginFunction(CheckerContext &C) const {
  ProgramStateRef State = C.getState();

  // Clear tracked state at function entry.
  auto PM = State->get<PendingNetdevMap>();
  if (!PM.isEmpty()) {
    for (auto I = PM.begin(), E = PM.end(); I != E; ++I)
      State = State->remove<PendingNetdevMap>(I->first);
  }

  auto LM = State->get<NetdevLoopMap>();
  if (!LM.isEmpty()) {
    for (auto I = LM.begin(), E = LM.end(); I != E; ++I)
      State = State->remove<NetdevLoopMap>(I->first);
  }

  auto AM = State->get<PtrAliasMap>();
  if (!AM.isEmpty()) {
    for (auto I = AM.begin(), E = AM.end(); I != E; ++I)
      State = State->remove<PtrAliasMap>(I->first);
  }

  C.addTransition(State);
}

static bool isPointerLikeRegion(const MemRegion *R) {
  return R != nullptr;
}

void SAGenTestChecker::checkBind(SVal Loc, SVal Val, const Stmt *StoreE,
                                 CheckerContext &C) const {
  ProgramStateRef State = C.getState();
  bool Changed = false;

  const MemRegion *DstR = Loc.getAsRegion();
  if (DstR)
    DstR = DstR->getBaseRegion();

  // Track simple pointer assignments as aliases.
  if (DstR && isPointerLikeRegion(DstR)) {
    if (const MemRegion *SrcR = Val.getAsRegion()) {
      SrcR = SrcR->getBaseRegion();
      if (SrcR) {
        State = State->set<PtrAliasMap>(DstR, SrcR);
        Changed = true;
      }
    }
  }

  // Track an allocation assigned to a pointer inside a loop.
  if (StoreE && DstR) {
    const CallExpr *CE = findSpecificTypeInChildren<CallExpr>(StoreE);
    if (CE && isAllocNetdevCall(CE, C)) {
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

void SAGenTestChecker::checkPostCall(const CallEvent &Call,
                                     CheckerContext &C) const {
  ProgramStateRef State = C.getState();
  const Expr *Origin = Call.getOriginExpr();
  if (!Origin)
    return;

  // A successful register_netdev call transfers ownership.
  if (ExprHasName(Origin, "register_netdev", C)) {
    if (Call.getNumArgs() >= 1) {
      const Expr *Arg0 = Call.getArgExpr(0);
      if (Arg0) {
        const MemRegion *ArgR = getMemRegionFromExpr(Arg0, C);
        if (ArgR) {
          ArgR = canonicalizeRegion(State, ArgR->getBaseRegion());
          if (ArgR && (State->get<NetdevLoopMap>(ArgR) ||
                       State->get<PendingNetdevMap>(ArgR))) {
            erasePendingFor(State, ArgR);
            C.addTransition(State);
          }
        }
      }
    }
    return;
  }

  // free_netdev ends the tracked allocation's lifetime.
  if (ExprHasName(Origin, "free_netdev", C)) {
    if (Call.getNumArgs() >= 1) {
      const Expr *Arg0 = Call.getArgExpr(0);
      if (Arg0) {
        const MemRegion *ArgR = getMemRegionFromExpr(Arg0, C);
        if (ArgR) {
          ArgR = canonicalizeRegion(State, ArgR->getBaseRegion());
          if (ArgR && (State->get<NetdevLoopMap>(ArgR) ||
                       State->get<PendingNetdevMap>(ArgR))) {
            erasePendingFor(State, ArgR);
            C.addTransition(State);
          }
        }
      }
    }
  }
}

void SAGenTestChecker::checkBranchCondition(const Stmt *Condition,
                                             CheckerContext &C) const {
  if (!Condition)
    return;

  const IfStmt *IS = findSpecificTypeInParents<IfStmt>(Condition, C);
  if (!IS)
    return;

  const Stmt *Then = IS->getThen();
  if (!Then)
    return;

  GotoCollectorVisitor GV;
  GV.TraverseStmt(const_cast<Stmt *>(Then));

  bool HasExitLikeGoto = false;
  for (const GotoStmt *GS : GV.Gotos) {
    if (!GS)
      continue;

    if (isExitLikeLabel(GS->getLabel())) {
      HasExitLikeGoto = true;
      break;
    }
  }

  if (!HasExitLikeGoto)
    return;

  const Stmt *LoopS = findEnclosingLoop(IS, C);
  if (!LoopS)
    return;

  ProgramStateRef State = C.getState();
  auto Pend = State->get<PendingNetdevMap>();
  if (Pend.isEmpty())
    return;

  for (auto I = Pend.begin(), E = Pend.end(); I != E; ++I) {
    const MemRegion *R = I->first;
    if (!R)
      continue;

    R = R->getBaseRegion();
    auto RLoop = State->get<NetdevLoopMap>(R);
    if (!RLoop || *RLoop != LoopS)
      continue;

    // Allocation failure reaches this branch with a null pointer. It is not
    // a leak: alloc_etherdev did not produce a net_device to free.
    if (thenBranchTestsRegionForNull(dyn_cast<Expr>(Condition), R, State, C))
      continue;

    if (thenContainsFreeOfRegion(Then, State, R, C))
      continue;

    ExplodedNode *N = C.generateNonFatalErrorNode();
    if (!N)
      return;

    auto Rpt = std::make_unique<PathSensitiveBugReport>(
        *BT, "Missing free_netdev before goto exit; leaks current net_device",
        N);
    Rpt->addRange(Then->getSourceRange());
    C.emitReport(std::move(Rpt));
  }
}

bool SAGenTestChecker::isAllocNetdevCall(const Expr *E, CheckerContext &C) {
  if (!E)
    return false;

  return ExprHasName(E, "alloc_etherdev", C) ||
         ExprHasName(E, "alloc_etherdev_mqs", C) ||
         ExprHasName(E, "alloc_netdev", C) ||
         ExprHasName(E, "alloc_netdev_mqs", C);
}

bool SAGenTestChecker::isExitLikeLabel(const LabelDecl *LD) {
  if (!LD)
    return false;

  StringRef Name = LD->getName();
  if (Name.empty())
    return false;

  std::string Lower = Name.lower();
  StringRef LRef(Lower);

  if (LRef == "exit" || LRef == "out" || LRef == "error")
    return true;

  return LRef.startswith("err") || LRef.startswith("error") ||
         LRef.startswith("out");
}

const Stmt *SAGenTestChecker::findEnclosingLoop(const Stmt *S,
                                                CheckerContext &C) {
  if (!S)
    return nullptr;

  if (const auto *FS = findSpecificTypeInParents<ForStmt>(S, C))
    return FS;
  if (const auto *WS = findSpecificTypeInParents<WhileStmt>(S, C))
    return WS;
  if (const auto *DS = findSpecificTypeInParents<DoStmt>(S, C))
    return DS;

  return nullptr;
}

} // namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects missing free_netdev before goto exit in loops (leaks current "
      "iteration net_device)",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
