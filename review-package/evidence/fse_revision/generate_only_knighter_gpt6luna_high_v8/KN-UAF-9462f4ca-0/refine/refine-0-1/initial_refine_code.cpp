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

// Track the regions for tx_lock instances held on the current path.
REGISTER_SET_WITH_PROGRAMSTATE(HeldTxLocks, const MemRegion*)

namespace {

static bool StmtContainsText(const Stmt *S, StringRef Needle,
                             const SourceManager &SM,
                             const LangOptions &LangOpts) {
  if (!S)
    return false;

  CharSourceRange R = CharSourceRange::getTokenRange(S->getSourceRange());
  StringRef Text = Lexer::getSourceText(R, SM, LangOpts);
  return Text.contains(Needle);
}

static bool StmtContainsText(const Stmt *S, StringRef Needle,
                             CheckerContext &C) {
  return StmtContainsText(S, Needle, C.getSourceManager(), C.getLangOpts());
}

static bool ContainsListAndTxTarget(const Stmt *S, CheckerContext &C) {
  if (!S)
    return false;

  const bool HasList =
      StmtContainsText(S, "list_for_each_entry_safe", C) ||
      StmtContainsText(S, "list_for_each_entry", C);
  const bool HasTxList =
      StmtContainsText(S, "tx_ctrl_list", C) ||
      StmtContainsText(S, "tx_data_list", C);

  return HasList && HasTxList;
}

static bool IsKfreeLike(const CallEvent &Call, CheckerContext &C) {
  const Expr *Origin = Call.getOriginExpr();
  if (!Origin)
    return false;

  if (const IdentifierInfo *ID = Call.getCalleeIdentifier()) {
    StringRef Name = ID->getName();
    if (Name == "kfree" || Name == "kvfree")
      return true;
  }

  return ExprHasName(Origin, "kfree", C) ||
         ExprHasName(Origin, "kvfree", C);
}

static bool IsSpinLockAcquire(const CallEvent &Call) {
  const IdentifierInfo *ID = Call.getCalleeIdentifier();
  if (!ID)
    return false;

  StringRef Name = ID->getName();
  return Name == "spin_lock" || Name == "spin_lock_irq" ||
         Name == "spin_lock_irqsave" || Name == "raw_spin_lock" ||
         Name == "raw_spin_lock_irq" ||
         Name == "raw_spin_lock_irqsave" || Name == "_raw_spin_lock" ||
         Name == "_raw_spin_lock_irq" ||
         Name == "_raw_spin_lock_irqsave" || Name == "__raw_spin_lock" ||
         Name == "__raw_spin_lock_irq" ||
         Name == "__raw_spin_lock_irqsave";
}

static bool IsSpinLockRelease(const CallEvent &Call) {
  const IdentifierInfo *ID = Call.getCalleeIdentifier();
  if (!ID)
    return false;

  StringRef Name = ID->getName();
  return Name == "spin_unlock" || Name == "spin_unlock_irq" ||
         Name == "spin_unlock_irqrestore" || Name == "raw_spin_unlock" ||
         Name == "raw_spin_unlock_irq" ||
         Name == "raw_spin_unlock_irqrestore" ||
         Name == "_raw_spin_unlock" ||
         Name == "_raw_spin_unlock_irq" ||
         Name == "_raw_spin_unlock_irqrestore" ||
         Name == "__raw_spin_unlock" ||
         Name == "__raw_spin_unlock_irq" ||
         Name == "__raw_spin_unlock_irqrestore";
}

class SAGenTestChecker : public Checker<check::PreCall> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Unsafe list free without tx_lock",
                       "Concurrency")) {}

  void checkPreCall(const CallEvent &Call, CheckerContext &C) const;

private:
  bool inTargetTraversalContext(const Stmt *CallOrigin,
                                CheckerContext &C) const;
  void handleLockAcquire(const CallEvent &Call, CheckerContext &C) const;
  void handleLockRelease(const CallEvent &Call, CheckerContext &C) const;
  void reportUnsafeFree(const CallEvent &Call, CheckerContext &C) const;
};

bool SAGenTestChecker::inTargetTraversalContext(
    const Stmt *CallOrigin, CheckerContext &C) const {
  if (!CallOrigin)
    return false;

  // A candidate free must be inside the loop that traverses the target list.
  // Do not use a function-wide fallback: that also classifies unrelated frees
  // elsewhere in functions that happen to contain such a loop.
  if (const ForStmt *FS =
          findSpecificTypeInParents<ForStmt>(CallOrigin, C))
    return ContainsListAndTxTarget(FS, C);

  if (const WhileStmt *WS =
          findSpecificTypeInParents<WhileStmt>(CallOrigin, C))
    return ContainsListAndTxTarget(WS, C);

  if (const DoStmt *DS =
          findSpecificTypeInParents<DoStmt>(CallOrigin, C))
    return ContainsListAndTxTarget(DS, C);

  return false;
}

void SAGenTestChecker::handleLockAcquire(const CallEvent &Call,
                                         CheckerContext &C) const {
  if (Call.getNumArgs() < 1)
    return;

  const Expr *Arg0 = Call.getArgExpr(0);
  if (!Arg0 || !ExprHasName(Arg0, "tx_lock", C))
    return;

  // Keep the field region, rather than collapsing it to the containing
  // structure. This distinguishes tx_lock instances belonging to different
  // mux objects.
  const MemRegion *MR = getMemRegionFromExpr(Arg0, C);
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

  C.addTransition(C.getState()->remove<HeldTxLocks>(MR));
}

void SAGenTestChecker::reportUnsafeFree(const CallEvent &Call,
                                        CheckerContext &C) const {
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto R = std::make_unique<PathSensitiveBugReport>(
      *BT,
      "Freeing tx_* list entries without holding tx_lock (possible UAF)", N);
  R->addRange(Call.getSourceRange());
  C.emitReport(std::move(R));
}

void SAGenTestChecker::checkPreCall(const CallEvent &Call,
                                    CheckerContext &C) const {
  if (IsSpinLockAcquire(Call)) {
    handleLockAcquire(Call, C);
    return;
  }

  if (IsSpinLockRelease(Call)) {
    handleLockRelease(Call, C);
    return;
  }

  if (!IsKfreeLike(Call, C))
    return;

  const Expr *Origin = Call.getOriginExpr();
  if (!inTargetTraversalContext(Origin, C))
    return;

  // The free is protected if its tx_lock instance is held on this path.
  if (C.getState()->get<HeldTxLocks>().isEmpty())
    reportUnsafeFree(Call, C);
}

} // namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects freeing tx_* list entries without holding tx_lock during list traversal (possible UAF)",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
