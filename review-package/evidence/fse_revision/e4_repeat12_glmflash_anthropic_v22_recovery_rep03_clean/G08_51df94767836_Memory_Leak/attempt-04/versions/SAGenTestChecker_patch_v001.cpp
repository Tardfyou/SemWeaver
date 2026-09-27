#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/StaticAnalyzer/Checkers/utility.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ParentMapContext.h"
#include "llvm/ADT/SmallVector.h"

using namespace clang;
using namespace ento;

namespace {

// A net_device allocation found inside a loop body: the iteration owns the
// device and must release it before abandoning the loop.
struct NetdevCandidate {
  const Expr *LhsE = nullptr;
  const VarDecl *VD = nullptr;
};

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

// Collects LabelStmt inside a statement subtree.
struct LabelCollectorVisitor
    : public RecursiveASTVisitor<LabelCollectorVisitor> {
  SmallVector<const LabelStmt *, 8> Labels;
  bool VisitLabelStmt(LabelStmt *LS) {
    Labels.push_back(LS);
    return true;
  }
};

// Recognizes Linux net_device allocation helpers.
static bool isAllocNetdevCall(const Expr *E, CheckerContext &C) {
  if (!E) return false;
  return ExprHasName(E, "alloc_etherdev", C) ||
         ExprHasName(E, "alloc_etherdev_mqs", C) ||
         ExprHasName(E, "alloc_netdev", C) ||
         ExprHasName(E, "alloc_netdev_mqs", C);
}

// Single AST parent of S, or null when S has no statement parent.
static const Stmt *getSingleParent(const Stmt *S, CheckerContext &C) {
  if (!S) return nullptr;
  auto Parents = C.getASTContext().getParents(*S);
  if (Parents.empty())
    return nullptr;
  return Parents[0].get<Stmt>();
}

// Innermost IfStmt that contains S in its subtree.
static const IfStmt *findOwningIf(const Stmt *S, CheckerContext &C) {
  const Stmt *Cur = S;
  while (Cur) {
    if (const auto *IS = dyn_cast<IfStmt>(Cur))
      return IS;
    Cur = getSingleParent(Cur, C);
  }
  return nullptr;
}

// Innermost enclosing For/While/Do statement of S.
static const Stmt *findEnclosingLoop(const Stmt *S, CheckerContext &C) {
  const Stmt *Cur = S;
  while (Cur) {
    if (isa<ForStmt>(Cur) || isa<WhileStmt>(Cur) || isa<DoStmt>(Cur))
      return Cur;
    Cur = getSingleParent(Cur, C);
  }
  return nullptr;
}

static const Stmt *getLoopBody(const Stmt *LoopS) {
  if (const auto *FS = dyn_cast<ForStmt>(LoopS))
    return FS->getBody();
  if (const auto *WS = dyn_cast<WhileStmt>(LoopS))
    return WS->getBody();
  if (const auto *DS = dyn_cast<DoStmt>(LoopS))
    return DS->getBody();
  return nullptr;
}

static const VarDecl *getVarDeclFromExpr(const Expr *E) {
  if (!E) return nullptr;
  const Expr *Stripped = E->IgnoreParenImpCasts();
  if (const auto *DRE = dyn_cast<DeclRefExpr>(Stripped))
    return dyn_cast<VarDecl>(DRE->getDecl());
  return nullptr;
}

// Collects net_device allocations assigned inside a loop body.
struct NetdevAllocCollector
    : public RecursiveASTVisitor<NetdevAllocCollector> {
  CheckerContext *Ctx = nullptr;
  SmallVector<NetdevCandidate, 8> Candidates;

  void addCandidate(const Expr *LhsE) {
    if (!LhsE || !Ctx)
      return;
    NetdevCandidate Cand;
    Cand.LhsE = LhsE;
    Cand.VD = getVarDeclFromExpr(LhsE);
    Candidates.push_back(Cand);
  }

  bool VisitBinaryOperator(BinaryOperator *BO) {
    if (!Ctx || BO->getOpcode() != BO_Assign)
      return true;
    const Expr *RHS = BO->getRHS();
    if (!RHS || !isAllocNetdevCall(RHS, *Ctx))
      return true;
    addCandidate(BO->getLHS());
    return true;
  }

  bool VisitDeclStmt(DeclStmt *DS) {
    if (!Ctx)
      return true;
    for (const Decl *D : DS->decls()) {
      const auto *VD = dyn_cast<VarDecl>(D);
      if (!VD)
        continue;
      const Expr *Init = VD->getInit();
      if (!Init || !isAllocNetdevCall(Init, *Ctx))
        continue;
      NetdevCandidate Cand;
      Cand.LhsE = nullptr;
      Cand.VD = VD;
      Candidates.push_back(Cand);
    }
    return true;
  }
};

// Does E denote the same object as the recorded candidate?
static bool matchesCandidate(const Expr *E, const NetdevCandidate &Cand,
                             CheckerContext &C) {
  if (!E) return false;
  if (Cand.VD) {
    const Expr *Stripped = E->IgnoreParenImpCasts();
    if (const auto *DRE = dyn_cast<DeclRefExpr>(Stripped))
      if (DRE->getDecl() == Cand.VD)
        return true;
  }
  if (!Cand.LhsE) return false;
  const MemRegion *RE = getMemRegionFromExpr(E, C);
  const MemRegion *RC = getMemRegionFromExpr(Cand.LhsE, C);
  if (!RE || !RC) return false;
  return RE->getBaseRegion() == RC->getBaseRegion();
}

// True when the branch condition itself null-guards the candidate pointer:
// on that error path the allocation never happened, so nothing can leak.
static bool isNullGuardFor(const Stmt *Condition, const NetdevCandidate &Cand,
                           CheckerContext &C) {
  const auto *CondE = dyn_cast<Expr>(Condition);
  if (!CondE)
    return false;
  CondE = CondE->IgnoreParenImpCasts();

  if (const auto *UO = dyn_cast<UnaryOperator>(CondE)) {
    if (UO->getOpcode() != UO_LNot)
      return false;
    return matchesCandidate(UO->getSubExpr(), Cand, C);
  }

  if (const auto *BO = dyn_cast<BinaryOperator>(CondE)) {
    BinaryOperatorKind Op = BO->getOpcode();
    if (Op != BO_EQ && Op != BO_NE)
      return false;
    const Expr *LHS = BO->getLHS();
    const Expr *RHS = BO->getRHS();
    if (!LHS || !RHS)
      return false;
    ASTContext &AC = C.getASTContext();
    bool LNull = LHS->IgnoreParenImpCasts()->isNullPointerConstant(
                     AC, Expr::NPC_ValueDependentIsNull) != Expr::NPCK_NotNull;
    bool RNull = RHS->IgnoreParenImpCasts()->isNullPointerConstant(
                     AC, Expr::NPC_ValueDependentIsNull) != Expr::NPCK_NotNull;
    if (LNull && !RNull)
      return matchesCandidate(RHS, Cand, C);
    if (RNull && !LNull)
      return matchesCandidate(LHS, Cand, C);
  }
  return false;
}

// True when Then releases the candidate device with free_netdev before the
// exit path.
static bool thenContainsFreeOfCandidate(const Stmt *Then,
                                        const NetdevCandidate &Cand,
                                        CheckerContext &C) {
  if (!Then)
    return false;
  CallCollectorVisitor V;
  V.TraverseStmt(const_cast<Stmt *>(Then));
  for (const CallExpr *CE : V.Calls) {
    if (!CE || CE->getNumArgs() < 1)
      continue;
    if (!ExprHasName(CE, "free_netdev", C))
      continue;
    if (matchesCandidate(CE->getArg(0), Cand, C))
      return true;
  }
  return false;
}

// register_netdev hands the device over to the networking core: once it has
// been called on this device earlier in the loop body, the error exit path no
// longer owns the iteration allocation.
static bool ownershipTransferredBefore(const Stmt *LoopBody, const Stmt *Site,
                                       const NetdevCandidate &Cand,
                                       CheckerContext &C) {
  if (!LoopBody || !Site)
    return false;
  CallCollectorVisitor V;
  V.TraverseStmt(const_cast<Stmt *>(LoopBody));
  for (const CallExpr *CE : V.Calls) {
    if (!CE || CE->getNumArgs() < 1)
      continue;
    if (!ExprHasName(CE, "register_netdev", C))
      continue;
    if (ExprHasName(CE, "unregister_netdev", C))
      continue;
    if (!matchesCandidate(CE->getArg(0), Cand, C))
      continue;
    if (C.getSourceManager().isBeforeInTranslationUnit(CE->getBeginLoc(),
                                                       Site->getBeginLoc()))
      return true;
  }
  return false;
}

// True when Then contains a goto whose label lies outside the loop: the error
// path abandons the whole iteration.
static bool thenExitsLoop(const Stmt *Then, const Stmt *LoopS,
                          CheckerContext &C) {
  if (!Then || !LoopS)
    return false;
  GotoCollectorVisitor GV;
  GV.TraverseStmt(const_cast<Stmt *>(Then));
  if (GV.Gotos.empty())
    return false;

  const LocationContext *LC = C.getLocationContext();
  if (!LC)
    return false;
  const auto *FD = dyn_cast_or_null<FunctionDecl>(LC->getDecl());
  if (!FD || !FD->getBody())
    return false;

  LabelCollectorVisitor LV;
  LV.TraverseStmt(const_cast<Stmt *>(FD->getBody()));

  for (const GotoStmt *GS : GV.Gotos) {
    if (!GS)
      continue;
    const LabelDecl *LD = GS->getLabel();
    if (!LD)
      continue;
    for (const LabelStmt *LS : LV.Labels) {
      if (LS && LS->getDecl() == LD) {
        if (!LoopS->getSourceRange().fullyContains(LS->getSourceRange()))
          return true;
        break;
      }
    }
  }
  return false;
}

class SAGenTestChecker : public Checker<check::BranchCondition> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "net_device allocated in loop leaked on error exit",
                       "Memory Management")) {}

  void checkBranchCondition(const Stmt *Condition, CheckerContext &C) const;

};





void SAGenTestChecker::checkBranchCondition(const Stmt *Condition,
                                            CheckerContext &C) const {
  if (!Condition)
    return;

  // Error branch inside a loop.
  const IfStmt *IS = findOwningIf(Condition, C);
  if (!IS)
    return;

  const Stmt *Then = IS->getThen();
  if (!Then)
    return;

  const Stmt *LoopS = findEnclosingLoop(IS, C);
  if (!LoopS)
    return;

  // The error path must abandon the loop entirely.
  if (!thenExitsLoop(Then, LoopS, C))
    return;

  const Stmt *LoopBody = getLoopBody(LoopS);
  if (!LoopBody)
    return;

  // Devices allocated inside this loop body are iteration-local: an error
  // exit out of the loop must release the ones the iteration still owns.
  NetdevAllocCollector AC;
  AC.Ctx = &C;
  AC.TraverseStmt(const_cast<Stmt *>(LoopBody));
  if (AC.Candidates.empty())
    return;

  for (const NetdevCandidate &Cand : AC.Candidates) {
    // The allocation-failure guard itself: the pointer is null there, so no
    // device exists to release.
    if (isNullGuardFor(Condition, Cand, C))
      continue;
    // Released before exiting: no leak on this path.
    if (thenContainsFreeOfCandidate(Then, Cand, C))
      continue;
    // Ownership already handed to the networking core earlier in this
    // iteration: the exit path no longer owns the device.
    if (ownershipTransferredBefore(LoopBody, IS, Cand, C))
      continue;

    ExplodedNode *N = C.generateNonFatalErrorNode();
    if (!N)
      return;
    // Anchor the report at the abandonment point: the error exit that leaves
    // the loop is the sink where this iteration's still-owned net_device
    // escapes without free_netdev (ownership not yet transferred to the
    // networking core). The warning follows that exit statement wherever the
    // allocation and the guard happen to be written.
    SourceRange Anchor;
    GotoCollectorVisitor GV;
    GV.TraverseStmt(const_cast<Stmt *>(Then));
    if (!GV.Gotos.empty() && GV.Gotos.front())
      Anchor = GV.Gotos.front()->getSourceRange();
    else if (Cand.LhsE)
      Anchor = Cand.LhsE->getSourceRange();
    else if (Cand.VD)
      Anchor = Cand.VD->getSourceRange();
    else
      Anchor = Then->getSourceRange();
    auto Rpt = std::make_unique<PathSensitiveBugReport>(
        *BT,
        "net_device allocated in this loop is leaked on the error exit path",
        N);
    Rpt->addRange(Anchor);
    C.emitReport(std::move(Rpt));
    return;
  }
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
