// /external/upstream-study/SAGEN/result-0224-bugfail-multi-o3mini/test-Double-Free-39b13dce1a91cdfc3bec9238f9e89094551bd428/checkers/checker0.cpp
#include "clang/AST/Stmt.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/Lex/Lexer.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtVisitor.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/StaticAnalyzer/Checkers/utility.h"
#include "clang/AST/Expr.h"

using namespace clang;
using namespace ento;
using namespace taint;

namespace {

class SAGenTestChecker : public Checker<check::PreCall> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker() 
    : BT(new BugType(this, "Redundant Cleanup Call", "Double Free")) {}

  void checkPreCall(const CallEvent &Call, CheckerContext &C) const;

private:
  /// Report the error when a redundant cleanup call is detected.
  void reportRedundantCleanup(const CallEvent &Call, CheckerContext &C) const;
};

namespace {

static bool refersToSameObject(const Expr *First, const Expr *Second) {
  First = First->IgnoreParenImpCasts();
  Second = Second->IgnoreParenImpCasts();
  if (First == Second)
    return true;

  const auto *FirstRef = dyn_cast<DeclRefExpr>(First);
  const auto *SecondRef = dyn_cast<DeclRefExpr>(Second);
  return FirstRef && SecondRef && FirstRef->getDecl() == SecondRef->getDecl();
}

static bool isResetActionFailureBranch(const Expr *Origin,
                                       const CallExpr *Cleanup,
                                       const FunctionDecl *CleanupDecl,
                                       ASTContext &ACtx) {
  const Stmt *Child = Origin;
  while (true) {
    auto Parents = ACtx.getParents(*Child);
    if (Parents.empty())
      return false;

    const Stmt *Parent = Parents[0].get<Stmt>();
    if (!Parent)
      return false;

    const auto *If = dyn_cast<IfStmt>(Parent);
    if (If && Child == If->getThen()) {
      const Expr *Condition = If->getCond()->IgnoreParenImpCasts();
      const auto *Registration = dyn_cast<CallExpr>(Condition);
      if (Registration && Registration->getNumArgs() >= 3) {
        const FunctionDecl *RegistrationDecl = Registration->getDirectCallee();
        const auto *Callback = dyn_cast<DeclRefExpr>(
            Registration->getArg(1)->IgnoreParenImpCasts());
        const auto *CallbackDecl = Callback
            ? dyn_cast<FunctionDecl>(Callback->getDecl())
            : nullptr;
        if (RegistrationDecl &&
            RegistrationDecl->getName() == "devm_add_action_or_reset" &&
            CallbackDecl == CleanupDecl &&
            refersToSameObject(Registration->getArg(2), Cleanup->getArg(0)))
          return true;
      }
    }

    Child = Parent;
  }
}

} // end anonymous namespace

void SAGenTestChecker::checkPreCall(const CallEvent &Call, CheckerContext &C) const {
  const Expr *OriginExpr = Call.getOriginExpr();
  const auto *Cleanup = dyn_cast_or_null<CallExpr>(OriginExpr);
  if (!Cleanup || Cleanup->getNumArgs() != 1)
    return;

  const FunctionDecl *CleanupDecl = Cleanup->getDirectCallee();
  if (!CleanupDecl)
    return;

  if (!isResetActionFailureBranch(OriginExpr, Cleanup, CleanupDecl,
                                  C.getASTContext()))
    return;

  reportRedundantCleanup(Call, C);
}

void SAGenTestChecker::reportRedundantCleanup(const CallEvent &Call, CheckerContext &C) const {
  // Generate a non-fatal error node.
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  // Report the bug with a clear and short diagnostic message.
  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT, "Redundant cleanup call leads to double free", N);
  Report->addRange(Call.getSourceRange());
  C.emitReport(std::move(Report));
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker", 
      "Detects redundant cleanup call in error handling leading to double free", "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
