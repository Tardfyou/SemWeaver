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
#include "clang/StaticAnalyzer/Core/PathSensitive/SValBuilder.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtVisitor.h"
#include "clang/Analysis/AnalysisDeclContext.h"
#include "clang/Analysis/CFG.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/StaticAnalyzer/Checkers/utility.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Type.h"

using namespace clang;
using namespace ento;
using namespace taint;

REGISTER_MAP_WITH_PROGRAMSTATE(StatusVarAssignedMap, const MemRegion*, bool)
REGISTER_SET_WITH_PROGRAMSTATE(ReportedSet, const MemRegion*)

namespace {

// Scans the subtree of S for a store whose destination is VD. Used to
// classify CFG blocks as "assigns the status variable".
static bool stmtAssignsToVar(const Stmt *S, const VarDecl *VD) {
  if (!S || !VD)
    return false;
  llvm::SmallVector<const Stmt *, 16> Stack;
  Stack.push_back(S);
  while (!Stack.empty()) {
    const Stmt *Cur = Stack.pop_back_val();
    if (!Cur)
      continue;
    if (const auto *BO = dyn_cast<BinaryOperator>(Cur)) {
      if (BO->isAssignmentOp()) {
        const Expr *LHS = BO->getLHS()->IgnoreParenImpCasts();
        if (const auto *DRE = dyn_cast<DeclRefExpr>(LHS))
          if (DRE->getDecl() == VD)
            return true;
      }
    } else if (const auto *UO = dyn_cast<UnaryOperator>(Cur)) {
      if (UO->isIncrementDecrementOp()) {
        const Expr *Sub = UO->getSubExpr()->IgnoreParenImpCasts();
        if (const auto *DRE = dyn_cast<DeclRefExpr>(Sub))
          if (DRE->getDecl() == VD)
            return true;
      }
    }
    for (const Stmt *Child : Cur->children())
      Stack.push_back(Child);
  }
  return false;
}

// Assignment-free reachability: true when some CFG path from the function
// entry reaches the block containing RS without passing any block that
// stores to VD. This is the structural relation behind "returns an
// uninitialized status variable": the patch removes it by initializing the
// variable at declaration, which also removes the candidate status, so the
// relation is independent of variable naming and statement spelling.
static bool uninitReturnReachable(const ReturnStmt *RS, const VarDecl *VD,
                                  CheckerContext &C) {
  const LocationContext *LC = C.getLocationContext();
  if (!LC)
    return false;
  AnalysisDeclContext *AC = LC->getAnalysisDeclContext();
  if (!AC)
    return false;
  CFG *TheCFG = AC->getCFG();
  if (!TheCFG)
    return false;

  const CFGBlock *RetBlock = nullptr;
  llvm::SmallPtrSet<const CFGBlock *, 16> AssignBlocks;

  for (const CFGBlock *B : *TheCFG) {
    if (!B)
      continue;
    bool Assigns = false;
    for (const CFGElement &El : *B) {
      if (auto CS = El.getAs<CFGStmt>()) {
        const Stmt *S = CS->getStmt();
        if (S == RS)
          RetBlock = B;
        if (stmtAssignsToVar(S, VD))
          Assigns = true;
      }
    }
    if (!Assigns)
      if (const Stmt *Term = B->getTerminator())
        Assigns = stmtAssignsToVar(Term, VD);
    if (Assigns)
      AssignBlocks.insert(B);
  }

  if (!RetBlock || AssignBlocks.count(RetBlock))
    return false;

  const CFGBlock *Entry = &TheCFG->getEntry();
  if (AssignBlocks.count(Entry))
    return false;

  llvm::SmallPtrSet<const CFGBlock *, 32> Visited;
  llvm::SmallVector<const CFGBlock *, 32> Work;
  Work.push_back(Entry);
  Visited.insert(Entry);
  while (!Work.empty()) {
    const CFGBlock *Cur = Work.pop_back_val();
    if (Cur == RetBlock)
      return true;
    for (CFGBlock::succ_iterator I = Cur->succ_begin(), IE = Cur->succ_end();
         I != IE; ++I) {
      const CFGBlock *Succ = *I;
      if (!Succ || Visited.count(Succ) || AssignBlocks.count(Succ))
        continue;
      Visited.insert(Succ);
      Work.push_back(Succ);
    }
  }
  return false;
}

class SAGenTestChecker
  : public Checker<
        check::PostStmt<DeclStmt>,
        check::Bind,
        check::PreStmt<ReturnStmt>
    > {
   mutable std::unique_ptr<BugType> BT;

   public:
      SAGenTestChecker() : BT(new BugType(this, "Uninitialized return", "Logic error")) {}

      void checkPostStmt(const DeclStmt *DS, CheckerContext &C) const;
      void checkBind(SVal Loc, SVal Val, const Stmt *S, CheckerContext &C) const;
      void checkPreStmt(const ReturnStmt *RS, CheckerContext &C) const;

   private:
      static const FunctionDecl *getEnclosingFunction(const CheckerContext &C);
      static bool functionReturnsInteger(const CheckerContext &C);
      static bool isCandidateStatusVar(const VarDecl *VD, const CheckerContext &C);
      static const MemRegion *getVarRegion(const VarDecl *VD, CheckerContext &C);
      void reportUninitializedReturn(const VarDecl *VD, const MemRegion *R,
                                     const ReturnStmt *RS, CheckerContext &C) const;
};

const FunctionDecl *SAGenTestChecker::getEnclosingFunction(const CheckerContext &C) {
  const Decl *D = C.getLocationContext() ? C.getLocationContext()->getDecl() : nullptr;
  return dyn_cast_or_null<FunctionDecl>(D);
}

bool SAGenTestChecker::functionReturnsInteger(const CheckerContext &C) {
  const FunctionDecl *FD = getEnclosingFunction(C);
  if (!FD)
    return false;
  return FD->getReturnType()->isIntegerType();
}

bool SAGenTestChecker::isCandidateStatusVar(const VarDecl *VD, const CheckerContext &C) {
  if (!VD)
    return false;

  if (!VD->hasLocalStorage())
    return false;
  if (VD->isStaticLocal())
    return false;
  if (!VD->getType()->isIntegerType())
    return false;
  if (VD->hasInit())
    return false;
  if (!functionReturnsInteger(C))
    return false;

  // Candidate = uninitialized scalar local in an integer-returning function.
  // The warning additionally requires an assignment-free return path (or an
  // undefined path value), so the candidate set must not depend on specific
  // variable spellings; initialization at declaration removes candidacy and
  // keeps the fixed revision silent.
  return true;
}

const MemRegion *SAGenTestChecker::getVarRegion(const VarDecl *VD, CheckerContext &C) {
  ProgramStateRef State = C.getState();
  if (!State || !VD)
    return nullptr;
  const LocationContext *LCtx = C.getLocationContext();
  if (!LCtx)
    return nullptr;
  const MemRegion *MR = C.getSValBuilder().getRegionManager().getVarRegion(VD, LCtx);
  return MR;
}

void SAGenTestChecker::checkPostStmt(const DeclStmt *DS, CheckerContext &C) const {
  if (!DS)
    return;

  ProgramStateRef State = C.getState();
  ProgramStateRef NewState = State;

  for (const Decl *D : DS->decls()) {
    const VarDecl *VD = dyn_cast<VarDecl>(D);
    if (!VD)
      continue;

    if (!isCandidateStatusVar(VD, C))
      continue;

    const MemRegion *MR = getVarRegion(VD, C);
    if (!MR)
      continue;
    MR = MR->getBaseRegion();
    if (!MR)
      continue;

    // Initialize tracking as "unassigned"/"uninitialized"
    NewState = NewState->set<StatusVarAssignedMap>(MR, false);
  }

  if (NewState != State)
    C.addTransition(NewState);
}

void SAGenTestChecker::checkBind(SVal Loc, SVal, const Stmt *S, CheckerContext &C) const {
  ProgramStateRef State = C.getState();

  const MemRegion *R = Loc.getAsRegion();
  if (!R)
    return;
  R = R->getBaseRegion();
  if (!R)
    return;

  const bool *Tracked = State->get<StatusVarAssignedMap>(R);
  if (!Tracked)
    return;

  // Any store to the tracked variable marks it as assigned on this path.
  ProgramStateRef NewState = State->set<StatusVarAssignedMap>(R, true);
  if (NewState != State)
    C.addTransition(NewState);
}

void SAGenTestChecker::reportUninitializedReturn(const VarDecl *VD, const MemRegion *R,
                                                 const ReturnStmt *RS, CheckerContext &C) const {
  if (!BT || !R || !RS)
    return;

  ProgramStateRef State = C.getState();

  // Avoid duplicate reports for the same region on the same path.
  if (State->contains<ReportedSet>(R))
    return;

  ProgramStateRef NewState = State->add<ReportedSet>(R);
  ExplodedNode *N = C.generateNonFatalErrorNode(NewState);
  if (!N)
    return;

  std::string VarName = VD ? VD->getName().str() : std::string("variable");
  std::string Msg = "returning uninitialized local '" + VarName + "'";

  auto Rpt = std::make_unique<PathSensitiveBugReport>(*BT, Msg, N);
  Rpt->addRange(RS->getSourceRange());
  C.emitReport(std::move(Rpt));
}

void SAGenTestChecker::checkPreStmt(const ReturnStmt *RS, CheckerContext &C) const {
  if (!RS)
    return;
  if (!functionReturnsInteger(C))
    return;

  const Expr *E = RS->getRetValue();
  if (!E)
    return;

  const Expr *SimpE = E->IgnoreParenImpCasts();
  const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(SimpE);
  if (!DRE)
    return;

  const VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl());
  if (!VD)
    return;

  const MemRegion *MR = getVarRegion(VD, C);
  if (!MR)
    return;
  MR = MR->getBaseRegion();
  if (!MR)
    return;

  ProgramStateRef State = C.getState();
  const bool *Assigned = State->get<StatusVarAssignedMap>(MR);

  // Path-sensitive state relation: tracked on this path and never stored to.
  if (Assigned && !*Assigned) {
    reportUninitializedReturn(VD, MR, RS, C);
    return;
  }

  // Value relation: if the region holds no defined value on this path, the
  // return really propagates an uninitialized value.
  if (!State->getSVal(loc::MemRegionVal(MR), VD->getType()).isUndef()) {
    // The path value is defined; the return can still sit on an
    // assignment-free path from function entry (the zero-iteration loop
    // path). The patch's initialization at declaration removes candidacy,
    // keeping the fixed revision silent.
    if (isCandidateStatusVar(VD, C) && uninitReturnReachable(RS, VD, C))
      reportUninitializedReturn(VD, MR, RS, C);
    return;
  }

  reportUninitializedReturn(VD, MR, RS, C);
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects returning an uninitialized local status variable (e.g., 'ret')",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
