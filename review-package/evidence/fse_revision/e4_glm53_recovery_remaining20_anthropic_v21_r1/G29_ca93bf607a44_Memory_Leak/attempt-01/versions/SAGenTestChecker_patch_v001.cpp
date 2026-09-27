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
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"

using namespace clang;
using namespace ento;
using namespace taint;

// Program state customizations
REGISTER_SET_WITH_PROGRAMSTATE(MustFreeTemps, const MemRegion*)
REGISTER_MAP_WITH_PROGRAMSTATE(AllocSiteMap, const MemRegion*, const Stmt*)
REGISTER_SET_WITH_PROGRAMSTATE(DevmReallocDstSet, const MemRegion*)

namespace {

// Find the nearest ancestor of type T through the AST parent map. Non-Stmt
// parents (for example the VarDecl owning an initializer) are traversed as
// well, so an initializer CallExpr can still reach its DeclStmt.
template <typename T>
const T *findSpecificTypeInParents(const Stmt *Start, CheckerContext &C) {
  if (!Start)
    return nullptr;
  const ParentMapContext &Parents = C.getASTContext().getParentMapContext();
  llvm::SmallVector<DynTypedNode, 8> Worklist;
  for (const auto &Parent : Parents.getParents(*Start))
    Worklist.push_back(Parent);
  while (!Worklist.empty()) {
    DynTypedNode Current = Worklist.pop_back_val();
    if (const T *Found = Current.get<T>())
      return Found;
    for (const auto &Parent : Parents.getParents(Current))
      Worklist.push_back(Parent);
  }
  return nullptr;
}

// Depth-first search for the first descendant of type T.
template <typename T>
const T *findSpecificTypeInChildren(const Stmt *Start) {
  if (!Start)
    return nullptr;
  for (const Stmt *Child : Start->children()) {
    if (!Child)
      continue;
    if (const T *Found = dyn_cast<T>(Child))
      return Found;
    if (const T *Nested = findSpecificTypeInChildren<T>(Child))
      return Nested;
  }
  return nullptr;
}

// The region an expression designates as an lvalue: value conversions are
// stripped so the tracked object's own region is returned, not the loaded
// pointer value stored in it.
static const MemRegion *getMemRegionFromExpr(const Expr *E,
                                             CheckerContext &C) {
  if (!E)
    return nullptr;
  return C.getSVal(E->IgnoreParenImpCasts()).getAsRegion();
}

// True when the call resolves directly to a function declaration with the
// given name; used only to classify producer/free/reallocation roles.
static bool callDeclHasName(const CallEvent &Call, llvm::StringRef Name) {
  const FunctionDecl *FD = dyn_cast_or_null<FunctionDecl>(Call.getDecl());
  return FD && FD->getName() == Name;
}

class SAGenTestChecker : public Checker<
  check::PostCall,
  check::Bind,
  check::PreStmt<ReturnStmt>
> {
   mutable std::unique_ptr<BugType> BT;

   public:
      SAGenTestChecker() : BT(new BugType(this, "Memory leak on devm_krealloc failure", "Resource Leak")) {}

      void checkPostCall(const CallEvent &Call, CheckerContext &C) const;
      void checkBind(SVal Loc, SVal Val, const Stmt *S, CheckerContext &C) const;
      void checkPreStmt(const ReturnStmt *RS, CheckerContext &C) const;

   private:

      // Helpers to classify calls
      bool isTempAllocLike(const CallEvent &Call, CheckerContext &C) const;
      bool isFreeLike(const CallEvent &Call, CheckerContext &C) const;
      bool isDevmKrealloc(const CallEvent &Call, CheckerContext &C) const;

      // Find LHS region of an assignment or init surrounding a call expression
      const MemRegion* getLHSRegionOfEnclosingAssignmentOrInit(const CallEvent &Call,
                                                               CheckerContext &C) const;

      // Parse a null check condition. Returns true if recognized.
      bool parseNullCheck(const Stmt *Cond, CheckerContext &C,
                          const Expr *&TestedExpr, bool &NullOnThen) const;

      // Utility: check whether Parent subtree contains Child
      bool containsStmt(const Stmt *Parent, const Stmt *Child) const;

      // Helper to check if the given CallExpr is a devm_krealloc
      bool callExprIsDevmKrealloc(const CallExpr *CE, CheckerContext &C) const;

      void reportLeak(const IfStmt *IS, const ReturnStmt *RS,
                      const MemRegion *TmpMR, const Stmt *AllocSite,
                      CheckerContext &C) const;
};

bool SAGenTestChecker::isTempAllocLike(const CallEvent &Call, CheckerContext &C) const {
  // Minimum handling: nvmem_cell_read. Optionally include more alloc-like routines.
  return callDeclHasName(Call, "nvmem_cell_read") ||
         callDeclHasName(Call, "kmalloc") ||
         callDeclHasName(Call, "kcalloc") ||
         callDeclHasName(Call, "krealloc") ||
         callDeclHasName(Call, "kstrdup") ||
         callDeclHasName(Call, "kvmalloc");
}

bool SAGenTestChecker::isFreeLike(const CallEvent &Call, CheckerContext &C) const {
  return callDeclHasName(Call, "kfree");
}

bool SAGenTestChecker::isDevmKrealloc(const CallEvent &Call, CheckerContext &C) const {
  return callDeclHasName(Call, "devm_krealloc");
}

const MemRegion* SAGenTestChecker::getLHSRegionOfEnclosingAssignmentOrInit(const CallEvent &Call,
                                                                           CheckerContext &C) const {
  const Expr *Origin = Call.getOriginExpr();
  if (!Origin)
    return nullptr;

  // Case 1: assignment like "LHS = call(...)"
  if (const auto *BO = findSpecificTypeInParents<BinaryOperator>(Origin, C)) {
    if (BO->isAssignmentOp()) {
      const Expr *LHS = BO->getLHS();
      if (!LHS)
        return nullptr;
      const MemRegion *MR = getMemRegionFromExpr(LHS, C);
      if (!MR)
        return nullptr;
      return MR->getBaseRegion();
    }
  }

  // Case 2: declaration with initializer: "type var = call(...);"
  if (const auto *DS = findSpecificTypeInParents<DeclStmt>(Origin, C)) {
    if (DS->isSingleDecl()) {
      if (const auto *VD = dyn_cast<VarDecl>(DS->getSingleDecl())) {
        if (VD->hasInit()) {
          ProgramStateRef State = C.getState();
          const LocationContext *LCtx = C.getLocationContext();
          SVal LVal = State->getLValue(VD, LCtx);
          if (const MemRegion *MR = LVal.getAsRegion()) {
            return MR->getBaseRegion();
          }
        }
      }
    }
  }

  return nullptr;
}

bool SAGenTestChecker::parseNullCheck(const Stmt *Cond, CheckerContext &C,
                                      const Expr *&TestedExpr, bool &NullOnThen) const {
  TestedExpr = nullptr;
  NullOnThen = false;

  if (!Cond)
    return false;

  const Expr *E = dyn_cast<Expr>(Cond);
  if (!E)
    return false;

  E = E->IgnoreParenCasts();

  // if (!X)
  if (const auto *UO = dyn_cast<UnaryOperator>(E)) {
    if (UO->getOpcode() == UO_LNot) {
      TestedExpr = UO->getSubExpr();
      NullOnThen = true;
      return true;
    }
  }

  // if (X == NULL) or (X != NULL) or with 0
  if (const auto *BO = dyn_cast<BinaryOperator>(E)) {
    BinaryOperator::Opcode Op = BO->getOpcode();
    if (Op == BO_EQ || Op == BO_NE) {
      const Expr *LHS = BO->getLHS()->IgnoreParenCasts();
      const Expr *RHS = BO->getRHS()->IgnoreParenCasts();

      bool LHSIsNull = LHS->isNullPointerConstant(C.getASTContext(),
                                                  Expr::NPC_ValueDependentIsNull);
      bool RHSIsNull = RHS->isNullPointerConstant(C.getASTContext(),
                                                  Expr::NPC_ValueDependentIsNull);

      if (LHSIsNull && !RHSIsNull) {
        TestedExpr = RHS;
        NullOnThen = (Op == BO_EQ);
        return true;
      }
      if (RHSIsNull && !LHSIsNull) {
        TestedExpr = LHS;
        NullOnThen = (Op == BO_EQ);
        return true;
      }
    }
  }

  // if (X)  -> then is non-null branch
  if (isa<DeclRefExpr>(E) || isa<MemberExpr>(E) || isa<UnaryOperator>(E) || isa<ArraySubscriptExpr>(E)) {
    TestedExpr = E;
    NullOnThen = false;
    return true;
  }

  return false;
}

bool SAGenTestChecker::containsStmt(const Stmt *Parent, const Stmt *Child) const {
  if (!Parent || !Child)
    return false;
  if (Parent == Child)
    return true;
  for (const Stmt *S : Parent->children()) {
    if (S && containsStmt(S, Child))
      return true;
  }
  return false;
}

bool SAGenTestChecker::callExprIsDevmKrealloc(const CallExpr *CE, CheckerContext &C) const {
  if (!CE) return false;
  const FunctionDecl *FD = CE->getDirectCallee();
  return FD && FD->getName() == "devm_krealloc";
}

void SAGenTestChecker::reportLeak(const IfStmt *IS, const ReturnStmt *RS,
                                  const MemRegion *TmpMR, const Stmt *AllocSite,
                                  CheckerContext &C) const {
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto R = std::make_unique<PathSensitiveBugReport>(
      *BT, "Missing kfree of temporary buffer when devm_krealloc fails", N);

  if (RS)
    R->addRange(RS->getSourceRange());

  const SourceManager &SM = C.getSourceManager();
  const LocationContext *LCtx = C.getLocationContext();

  if (AllocSite) {
    PathDiagnosticLocation Loc = PathDiagnosticLocation::createBegin(AllocSite, SM, LCtx);
    R->addNote("temporary buffer allocated here", Loc);
  }

  if (IS && IS->getCond()) {
    PathDiagnosticLocation Loc2 = PathDiagnosticLocation::createBegin(IS->getCond(), SM, LCtx);
    R->addNote("devm_krealloc NULL-check here", Loc2);
  }

  C.emitReport(std::move(R));
}

void SAGenTestChecker::checkPostCall(const CallEvent &Call, CheckerContext &C) const {
  ProgramStateRef State = C.getState();

  // Track temporary allocations: nvmem_cell_read / kmalloc-like
  if (isTempAllocLike(Call, C)) {
    const MemRegion *LHSReg = getLHSRegionOfEnclosingAssignmentOrInit(Call, C);
    if (LHSReg) {
      // Track that this region holds a temp that must be freed
      State = State->add<MustFreeTemps>(LHSReg);
      // Remember alloc site for notes
      if (const Stmt *S = Call.getOriginExpr())
        State = State->set<AllocSiteMap>(LHSReg, S);
      C.addTransition(State);
      return;
    }
  }

  // Track frees: kfree(x). Clear the obligation of the tracked variable
  // that the freed argument designates, matched through the argument's
  // lvalue region so the fixed-side kfree(efuse) actually retires the temp.
  if (isFreeLike(Call, C)) {
    if (Call.getNumArgs() >= 1) {
      const Expr *Arg0 = Call.getArgExpr(0);
      const MemRegion *MR = Arg0 ? getMemRegionFromExpr(Arg0, C) : nullptr;
      if (MR) {
        MR = MR->getBaseRegion();
        // Remove from MustFreeTemps and AllocSiteMap if present
        if (State->contains<MustFreeTemps>(MR)) {
          State = State->remove<MustFreeTemps>(MR);
          if (State->get<AllocSiteMap>(MR))
            State = State->remove<AllocSiteMap>(MR);
          C.addTransition(State);
        }
        return;
      }
    }
  }

  // Track devm_krealloc destinations
  if (isDevmKrealloc(Call, C)) {
    const MemRegion *DstReg = getLHSRegionOfEnclosingAssignmentOrInit(Call, C);
    if (DstReg) {
      State = State->add<DevmReallocDstSet>(DstReg);
      C.addTransition(State);
      return;
    }
  }
}

void SAGenTestChecker::checkBind(SVal Loc, SVal Val, const Stmt *S, CheckerContext &C) const {
  ProgramStateRef State = C.getState();

  const MemRegion *LHSReg = Loc.getAsRegion();
  if (!LHSReg) {
    C.addTransition(State);
    return;
  }
  LHSReg = LHSReg->getBaseRegion();
  if (!LHSReg) {
    C.addTransition(State);
    return;
  }

  // If LHS was marked as a recent devm_krealloc destination,
  // keep it only if this store is also a devm_krealloc call; otherwise, erase.
  if (State->contains<DevmReallocDstSet>(LHSReg)) {
    bool Keep = false;
    if (S) {
      // Find a CallExpr in the RHS of this store
      if (const CallExpr *CE = findSpecificTypeInChildren<CallExpr>(S)) {
        if (callExprIsDevmKrealloc(CE, C))
          Keep = true;
      }
    }
    if (!Keep) {
      State = State->remove<DevmReallocDstSet>(LHSReg);
    }
  }

  C.addTransition(State);
}

void SAGenTestChecker::checkPreStmt(const ReturnStmt *RS, CheckerContext &C) const {
  if (!RS)
    return;

  // Find enclosing IfStmt
  const IfStmt *IS = findSpecificTypeInParents<IfStmt>(RS, C);
  if (!IS)
    return;

  // Identify which branch contains the return
  bool InThen = containsStmt(IS->getThen(), RS);
  bool InElse = containsStmt(IS->getElse(), RS);
  if (!InThen && !InElse)
    return;

  // Parse condition to see if it's a null-check
  const Expr *Tested = nullptr;
  bool NullOnThen = false;
  if (!parseNullCheck(IS->getCond(), C, Tested, NullOnThen))
    return;

  // Are we in the NULL branch?
  bool InNullBranch = (InThen && NullOnThen) || (InElse && !NullOnThen);
  if (!InNullBranch)
    return;

  // Get the region being tested (must be the devm_krealloc destination)
  const MemRegion *TestedReg = getMemRegionFromExpr(Tested, C);
  if (!TestedReg)
    return;
  TestedReg = TestedReg->getBaseRegion();
  if (!TestedReg)
    return;

  ProgramStateRef State = C.getState();

  // Ensure the tested region is a destination of a recent devm_krealloc
  if (!State->contains<DevmReallocDstSet>(TestedReg))
    return;

  // There must be at least one live temp that must be freed
  auto Temps = State->get<MustFreeTemps>();
  if (Temps.isEmpty())
    return;

  // Report a leak for one of the temps (reduce noise)
  const MemRegion *LeakMR = nullptr;
  for (auto It = Temps.begin(); It != Temps.end(); ++It) {
    LeakMR = *It;
    break;
  }
  if (!LeakMR)
    return;

  const Stmt *AllocSite = nullptr;
  if (const Stmt *const *SAlloc = State->get<AllocSiteMap>(LeakMR))
    AllocSite = *SAlloc;

  reportLeak(IS, RS, LeakMR, AllocSite, C);
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects memory leak of temporary buffers when devm_krealloc fails without freeing the temp",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
