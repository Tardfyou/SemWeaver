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

// Track the base region of each tx_lock held on the current path.
REGISTER_SET_WITH_PROGRAMSTATE(HeldTxLocks, const MemRegion*)

namespace {

static bool StmtContainsText(const Stmt *S, StringRef Needle,
                             const SourceManager &SM,
                             const LangOptions &LangOpts) {
  if (!S)
    return false;

  CharSourceRange Range = CharSourceRange::getTokenRange(S->getSourceRange());
  StringRef Text = Lexer::getSourceText(Range, SM, LangOpts);
  return Text.contains(Needle);
}

static bool StmtContainsText(const Stmt *S, StringRef Needle,
                             CheckerContext &C) {
  return StmtContainsText(S, Needle, C.getSourceManager(), C.getLangOpts());
}

static bool StmtContainsTextAST(const Stmt *S, StringRef Needle,
                                ASTContext &ACtx) {
  return StmtContainsText(S, Needle, ACtx.getSourceManager(),
                          ACtx.getLangOpts());
}

static bool ContainsListAndTxTarget(const Stmt *S, CheckerContext &C) {
  if (!S)
    return false;

  const bool HasList =
      StmtContainsText(S, "list_for_each_entry_safe", C) ||
      StmtContainsText(S, "list_for_each_entry", C);
  const bool HasTxList = StmtContainsText(S, "tx_ctrl_list", C) ||
                         StmtContainsText(S, "tx_data_list", C);
  return HasList && HasTxList;
}

static bool IsKfreeLike(const CallEvent &Call, CheckerContext &C) {
  const Expr *Origin = Call.getOriginExpr();
  return Origin && (ExprHasName(Origin, "kfree", C) ||
                    ExprHasName(Origin, "kvfree", C));
}

static bool IsSpinLockAcquire(const CallEvent &Call, CheckerContext &C) {
  const Expr *Origin = Call.getOriginExpr();
  return Origin && (ExprHasName(Origin, "spin_lock", C) ||
                    ExprHasName(Origin, "raw_spin_lock", C));
}

static bool IsSpinLockRelease(const CallEvent &Call, CheckerContext &C) {
  const Expr *Origin = Call.getOriginExpr();
  return Origin && (ExprHasName(Origin, "spin_unlock", C) ||
                    ExprHasName(Origin, "raw_spin_unlock", C));
}

static const Stmt *FindTargetListLoop(const Stmt *CallOrigin,
                                      CheckerContext &C) {
  if (const ForStmt *FS = findSpecificTypeInParents<ForStmt>(CallOrigin, C))
    if (ContainsListAndTxTarget(FS, C))
      return FS;

  if (const WhileStmt *WS =
          findSpecificTypeInParents<WhileStmt>(CallOrigin, C))
    if (ContainsListAndTxTarget(WS, C))
      return WS;

  if (const DoStmt *DS = findSpecificTypeInParents<DoStmt>(CallOrigin, C))
    if (ContainsListAndTxTarget(DS, C))
      return DS;

  return nullptr;
}

// This is a conservative fallback for kernel locking macros that do not
// produce useful lock/unlock CallEvents. It only suppresses a report when
// the source shows protection surrounding this particular target-list loop.
static bool IsStructurallyProtected(const Stmt *CallOrigin,
                                    CheckerContext &C) {
  const Stmt *Loop = FindTargetListLoop(CallOrigin, C);
  if (!Loop)
    return false;

  const LocationContext *LCtx = C.getLocationContext();
  const auto *FD =
      dyn_cast_or_null<FunctionDecl>(LCtx ? LCtx->getDecl() : nullptr);
  if (!FD || !FD->getBody())
    return false;

  const Stmt *Body = FD->getBody();
  const SourceManager &SM = C.getSourceManager();
  const LangOptions &LangOpts = C.getLangOpts();

  CharSourceRange BodyRange =
      CharSourceRange::getTokenRange(Body->getSourceRange());
  StringRef BodyText = Lexer::getSourceText(BodyRange, SM, LangOpts);
  if (BodyText.empty())
    return false;

  SourceLocation BodyBegin = SM.getExpansionLoc(Body->getBeginLoc());
  SourceLocation LoopBegin = SM.getExpansionLoc(Loop->getBeginLoc());
  SourceLocation FreeBegin = SM.getExpansionLoc(CallOrigin->getBeginLoc());
  if (BodyBegin.isInvalid() || LoopBegin.isInvalid() || FreeBegin.isInvalid() ||
      BodyBegin.isMacroID() || LoopBegin.isMacroID() || FreeBegin.isMacroID())
    return false;

  unsigned BodyOffset = SM.getFileOffset(BodyBegin);
  unsigned LoopOffset = SM.getFileOffset(LoopBegin);
  unsigned FreeOffset = SM.getFileOffset(FreeBegin);

  if (LoopOffset < BodyOffset || FreeOffset < BodyOffset)
    return false;

  LoopOffset -= BodyOffset;
  FreeOffset -= BodyOffset;
  if (LoopOffset >= BodyText.size() || FreeOffset >= BodyText.size() ||
      LoopOffset > FreeOffset)
    return false;

  // A scoped guard remains held through the loop and is released by its
  // destructor. Require the guard to precede the loop.
  StringRef BeforeLoop = BodyText.take_front(LoopOffset);
  size_t GuardPos = BeforeLoop.rfind("guard");
  if (GuardPos != StringRef::npos) {
    StringRef GuardText = BeforeLoop.drop_front(GuardPos);
    if (GuardText.contains("spinlock_irqsave") &&
        GuardText.contains("tx_lock"))
      return true;
  }

  // For explicit lock calls, require an acquisition before the loop and a
  // release after this free. A release between acquisition and the loop
  // means the free is not covered.
  StringRef BeforeFree = BodyText.take_front(FreeOffset);
  size_t AcquirePos = BeforeFree.rfind("spin_lock");
  if (AcquirePos == StringRef::npos)
    AcquirePos = BeforeFree.rfind("raw_spin_lock");
  if (AcquirePos == StringRef::npos || AcquirePos >= LoopOffset)
    return false;

  StringRef LockText = BeforeFree.drop_front(AcquirePos);
  size_t TxLockPos = LockText.find("tx_lock");
  if (TxLockPos == StringRef::npos)
    return false;

  StringRef SinceAcquire = BeforeFree.drop_front(AcquirePos);
  if (SinceAcquire.contains("spin_unlock") ||
      SinceAcquire.contains("raw_spin_unlock"))
    return false;

  StringRef AfterFree = BodyText.drop_front(FreeOffset);
  size_t UnlockPos = AfterFree.find("spin_unlock");
  size_t RawUnlockPos = AfterFree.find("raw_spin_unlock");
  if (UnlockPos == StringRef::npos ||
      (RawUnlockPos != StringRef::npos && RawUnlockPos < UnlockPos))
    UnlockPos = RawUnlockPos;

  return UnlockPos != StringRef::npos;
}

} // namespace

class SAGenTestChecker : public Checker<check::PreCall, check::ASTCodeBody> {
  mutable std::unique_ptr<BugType> BT;
  mutable llvm::DenseSet<const FunctionDecl *> FnHasTxListLoop;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Unsafe list free without tx_lock",
                       "Concurrency")) {}

  void checkPreCall(const CallEvent &Call, CheckerContext &C) const;
  void checkASTCodeBody(const Decl *D, AnalysisManager &Mgr,
                        BugReporter &BR) const;

private:
  bool inTargetTraversalContext(const Stmt *CallOrigin,
                                CheckerContext &C) const;
  bool isFalsePositive(const Stmt *CallOrigin, CheckerContext &C) const;
  void handleLockAcquire(const CallEvent &Call, CheckerContext &C) const;
  void handleLockRelease(const CallEvent &Call, CheckerContext &C) const;
  void reportUnsafeFree(const CallEvent &Call, CheckerContext &C) const;
};

bool SAGenTestChecker::inTargetTraversalContext(const Stmt *CallOrigin,
                                                CheckerContext &C) const {
  if (!CallOrigin)
    return false;

  if (FindTargetListLoop(CallOrigin, C))
    return true;

  // Retain the function-level fallback for ASTs where macro expansion
  // prevents locating the loop as a parent of the call.
  const LocationContext *LCtx = C.getLocationContext();
  const auto *FD =
      dyn_cast_or_null<FunctionDecl>(LCtx ? LCtx->getDecl() : nullptr);
  return FD && FnHasTxListLoop.count(FD);
}

bool SAGenTestChecker::isFalsePositive(const Stmt *CallOrigin,
                                       CheckerContext &C) const {
  return IsStructurallyProtected(CallOrigin, C);
}

void SAGenTestChecker::handleLockAcquire(const CallEvent &Call,
                                         CheckerContext &C) const {
  if (Call.getNumArgs() < 1)
    return;

  const Expr *Arg0 = Call.getArgExpr(0);
  if (!Arg0 || !ExprHasName(Arg0, "tx_lock", C))
    return;

  const MemRegion *MR = getMemRegionFromExpr(Arg0, C);
  if (!MR)
    return;

  MR = MR->getBaseRegion();
  if (!MR)
    return;

  C.addTransition(C.getState()->add<HeldTxLocks>(MR));
}

void SAGenTestChecker::handleLockRelease(const CallEvent &Call,
                                         CheckerContext &C) const {
  if (Call.getNumArgs() < 1)
    return;

  const Expr *Arg0 = Call.getArgExpr(0);
  if (!Arg0 || !ExprHasName(Arg0, "tx_lock", C))
    return;

  const MemRegion *MR = getMemRegionFromExpr(Arg0, C);
  if (!MR)
    return;

  MR = MR->getBaseRegion();
  if (!MR)
    return;

  C.addTransition(C.getState()->remove<HeldTxLocks>(MR));
}

void SAGenTestChecker::reportUnsafeFree(const CallEvent &Call,
                                        CheckerContext &C) const {
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT, "Freeing tx_* list entries without holding tx_lock (possible UAF)",
      N);
  Report->addRange(Call.getSourceRange());
  C.emitReport(std::move(Report));
}

void SAGenTestChecker::checkPreCall(const CallEvent &Call,
                                    CheckerContext &C) const {
  const Expr *Origin = Call.getOriginExpr();
  if (!Origin)
    return;

  if (IsSpinLockAcquire(Call, C)) {
    handleLockAcquire(Call, C);
    return;
  }

  if (IsSpinLockRelease(Call, C)) {
    handleLockRelease(Call, C);
    return;
  }

  if (!IsKfreeLike(Call, C) || !inTargetTraversalContext(Origin, C))
    return;

  // The structural check covers lock macros and confirms that protection
  // applies to this loop, not merely somewhere in the containing function.
  if (isFalsePositive(Origin, C))
    return;

  if (C.getState()->get<HeldTxLocks>().isEmpty())
    reportUnsafeFree(Call, C);
}

void SAGenTestChecker::checkASTCodeBody(const Decl *D, AnalysisManager &Mgr,
                                        BugReporter &BR) const {
  const auto *FD = dyn_cast<FunctionDecl>(D);
  if (!FD || !FD->getBody())
    return;

  ASTContext &ACtx = Mgr.getASTContext();
  const Stmt *Body = FD->getBody();

  const bool HasList =
      StmtContainsTextAST(Body, "list_for_each_entry_safe", ACtx) ||
      StmtContainsTextAST(Body, "list_for_each_entry", ACtx);
  const bool HasTxList = StmtContainsTextAST(Body, "tx_ctrl_list", ACtx) ||
                         StmtContainsTextAST(Body, "tx_data_list", ACtx);

  if (HasList && HasTxList)
    FnHasTxListLoop.insert(FD);
}

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects freeing tx_* list entries without holding tx_lock during list "
      "traversal (possible UAF)",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
