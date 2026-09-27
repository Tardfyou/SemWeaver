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
#include "clang/Lex/Lexer.h"
#include "llvm/ADT/DenseSet.h"

using namespace clang;
using namespace ento;
using namespace taint;

// Program state: base regions of spin locks currently held on this path
REGISTER_SET_WITH_PROGRAMSTATE(HeldSpinLocks, const MemRegion*)

namespace {

// ---------------------------------------------------------------------------
// Lock side.  A spin-lock family acquisition marks the region of its lock
// argument as held; the matching release clears it.  This covers direct calls
// (spin_lock, spin_lock_irqsave, raw_spin_lock*, the _raw_* externs reached
// after inlining) and the RAII scope guards used by the fixed code, where
// guard(spinlock_*) lowers to a class_*_constructor call on the lock argument
// with class_*_destructor releasing it at scope exit.
// ---------------------------------------------------------------------------

static StringRef GetCalleeName(const CallEvent &Call) {
  if (const auto *FD = dyn_cast_or_null<FunctionDecl>(Call.getDecl()))
    return FD->getName();
  return StringRef();
}

static bool IsSpinLockAcquire(const CallEvent &Call) {
  StringRef N = GetCalleeName(Call);
  if (N.empty())
    return false;
  if (N.contains("unlock") || N.contains("trylock") || N.contains("assert") ||
      N.contains("is_lock") || N.contains("can_lock"))
    return false;
  if (N.contains("spin_lock"))
    return true;
  return N.contains("spinlock") && N.contains("constructor");
}

static bool IsSpinLockRelease(const CallEvent &Call) {
  StringRef N = GetCalleeName(Call);
  if (N.empty())
    return false;
  if (N.contains("spin_unlock"))
    return true;
  return N.contains("spinlock") && N.contains("destructor");
}

// The lock object is the value of the pointer argument, so direct calls,
// guard wrappers, and inlined frames that forward the lock pointer all
// resolve to the same lock region.
static const MemRegion *GetLockRegion(const CallEvent &Call) {
  if (Call.getNumArgs() < 1)
    return nullptr;
  const MemRegion *MR = Call.getArgSVal(0).getAsRegion();
  if (!MR)
    return nullptr;
  return MR->getBaseRegion();
}

static bool IsKfreeLike(const CallEvent &Call, CheckerContext &C) {
  const Expr *Origin = Call.getOriginExpr();
  if (!Origin) return false;
  return ExprHasName(Origin, "kfree", C) || ExprHasName(Origin, "kvfree", C);
}

// ---------------------------------------------------------------------------
// Traversal side.  Kernel list traversal advances cursors by following the
// next/prev members of an embedded struct list_head, and list_for_each_entry*
// expands to exactly that assignment shape inside a loop (whether container_of
// stays a macro or becomes an inline call).  A traversal cursor is therefore a
// variable assigned in a loop from an expression that follows such a link.
// ---------------------------------------------------------------------------

class ListLinkFollowFinder : public RecursiveASTVisitor<ListLinkFollowFinder> {
public:
  bool Found = false;

  bool VisitMemberExpr(MemberExpr *ME) {
    StringRef Field = ME->getMemberDecl()->getName();
    if (Field != "next" && Field != "prev")
      return true;
    const auto *PtrT = ME->getType().getTypePtr()->getAs<PointerType>();
    if (!PtrT)
      return true;
    const RecordType *RT = PtrT->getPointeeType()->getAs<RecordType>();
    if (!RT)
      return true;
    StringRef Rec = RT->getDecl()->getName();
    if (Rec == "list_head" || Rec == "hlist_node")
      Found = true;
    return true;
  }
};

static bool FollowsListLink(const Expr *E) {
  if (!E)
    return false;
  ListLinkFollowFinder F;
  F.TraverseStmt(const_cast<Expr *>(E));
  return F.Found;
}

class LoopCursorCollector : public RecursiveASTVisitor<LoopCursorCollector> {
  llvm::DenseSet<const VarDecl *> &Out;

public:
  LoopCursorCollector(llvm::DenseSet<const VarDecl *> &Out_) : Out(Out_) {}

  bool VisitBinaryOperator(BinaryOperator *B) {
    if (!B->isAssignmentOp())
      return true;
    const Expr *LHS = B->getLHS()->IgnoreParenImpCasts();
    const auto *DRE = dyn_cast<DeclRefExpr>(LHS);
    if (!DRE)
      return true;
    const auto *VD = dyn_cast<VarDecl>(DRE->getDecl());
    if (VD && FollowsListLink(B->getRHS()))
      Out.insert(VD);
    return true;
  }

  bool VisitDeclStmt(DeclStmt *S) {
    for (const Decl *D : S->decls()) {
      const auto *VD = dyn_cast<VarDecl>(D);
      if (VD && FollowsListLink(VD->getInit()))
        Out.insert(VD);
    }
    return true;
  }
};

static bool LoopCursorsContain(const Stmt *Loop, const VarDecl *Freed) {
  if (!Loop || !Freed)
    return false;
  llvm::DenseSet<const VarDecl *> Cursors;
  LoopCursorCollector Collector(Cursors);
  Collector.TraverseStmt(const_cast<Stmt *>(Loop));
  return Cursors.count(Freed) > 0;
}

// True when Freed is a traversal cursor of the loop enclosing the call.
static bool IsCursorOfEnclosingLoop(const VarDecl *Freed, const Stmt *CallOrigin,
                                    CheckerContext &C) {
  if (const ForStmt *FS = findSpecificTypeInParents<ForStmt>(CallOrigin, C))
    if (LoopCursorsContain(FS, Freed))
      return true;
  if (const WhileStmt *WS = findSpecificTypeInParents<WhileStmt>(CallOrigin, C))
    if (LoopCursorsContain(WS, Freed))
      return true;
  if (const DoStmt *DS = findSpecificTypeInParents<DoStmt>(CallOrigin, C))
    if (LoopCursorsContain(DS, Freed))
      return true;
  return false;
}

static const VarDecl *GetFreedVar(const CallEvent &Call) {
  if (Call.getNumArgs() < 1)
    return nullptr;
  const Expr *Arg = Call.getArgExpr(0);
  if (!Arg)
    return nullptr;
  Arg = Arg->IgnoreParenImpCasts();
  if (const auto *DRE = dyn_cast<DeclRefExpr>(Arg))
    return dyn_cast<VarDecl>(DRE->getDecl());
  return nullptr;
}

class SAGenTestChecker : public Checker<check::PreCall> {
   mutable std::unique_ptr<BugType> BT;

   public:
      SAGenTestChecker() : BT(new BugType(this, "Free of list traversal cursor without held lock", "Concurrency")) {}

      void checkPreCall(const CallEvent &Call, CheckerContext &C) const;

   private:
      void handleLockAcquire(const CallEvent &Call, CheckerContext &C) const;
      void handleLockRelease(const CallEvent &Call, CheckerContext &C) const;
      void reportUnsafeFree(const CallEvent &Call, CheckerContext &C) const;
};

void SAGenTestChecker::handleLockAcquire(const CallEvent &Call, CheckerContext &C) const {
  const MemRegion *MR = GetLockRegion(Call);
  if (!MR)
    return;
  ProgramStateRef State = C.getState();
  State = State->add<HeldSpinLocks>(MR);
  C.addTransition(State);
}

void SAGenTestChecker::handleLockRelease(const CallEvent &Call, CheckerContext &C) const {
  const MemRegion *MR = GetLockRegion(Call);
  if (!MR)
    return;
  ProgramStateRef State = C.getState();
  State = State->remove<HeldSpinLocks>(MR);
  C.addTransition(State);
}

void SAGenTestChecker::reportUnsafeFree(const CallEvent &Call, CheckerContext &C) const {
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N) return;

  auto R = std::make_unique<PathSensitiveBugReport>(
      *BT, "Freeing a list traversal cursor while the list lock is not held (possible UAF)", N);
  R->addRange(Call.getSourceRange());
  C.emitReport(std::move(R));
}

void SAGenTestChecker::checkPreCall(const CallEvent &Call, CheckerContext &C) const {
  const Expr *Origin = Call.getOriginExpr();
  if (!Origin)
    return;

  // Track spin lock acquire/release
  if (IsSpinLockAcquire(Call)) {
    handleLockAcquire(Call, C);
    return;
  }
  if (IsSpinLockRelease(Call)) {
    handleLockRelease(Call, C);
    return;
  }

  // Detect unsafe frees of list traversal cursors
  if (!IsKfreeLike(Call, C))
    return;

  const VarDecl *Freed = GetFreedVar(Call);
  if (!Freed)
    return;

  // Only frees of cursors of the list traversal loop enclosing the call
  if (!IsCursorOfEnclosingLoop(Freed, Origin, C))
    return;

  ProgramStateRef State = C.getState();
  auto Locks = State->get<HeldSpinLocks>();
  if (Locks.isEmpty()) {
    reportUnsafeFree(Call, C);
  }
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects freeing a list traversal cursor while no spin lock is held (possible UAF)",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
