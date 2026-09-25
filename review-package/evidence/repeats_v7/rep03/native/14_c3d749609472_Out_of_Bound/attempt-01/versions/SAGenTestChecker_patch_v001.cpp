// /external/upstream-study/SAGEN/result-0224-bugfail-multi-o3mini/test-Out-of-Bound-c3d749609472ba0b217b42ab66f80459847e2bcb/checkers/checker1.cpp
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
#include "clang/Lex/Lexer.h"  // Needed for Lexer utilities

using namespace clang;
using namespace ento;
using namespace taint;

namespace {

static const Expr *stripCasts(const Expr *E) {
  return E ? E->IgnoreParenImpCasts() : nullptr;
}

static bool isMaxLinksCapability(const Expr *E) {
  const auto *MaxLinks = dyn_cast_or_null<MemberExpr>(stripCasts(E));
  if (!MaxLinks || MaxLinks->getMemberDecl()->getName() != "max_links")
    return false;

  const auto *Caps = dyn_cast_or_null<MemberExpr>(stripCasts(MaxLinks->getBase()));
  return Caps && Caps->getMemberDecl()->getName() == "caps";
}

class IndexedSecureDisplayContextFinder
    : public RecursiveASTVisitor<IndexedSecureDisplayContextFinder> {
  const ValueDecl *Index;
  bool Found = false;

public:
  explicit IndexedSecureDisplayContextFinder(const ValueDecl *Index)
      : Index(Index) {}

  bool VisitArraySubscriptExpr(ArraySubscriptExpr *Access) {
    const auto *IndexRef = dyn_cast_or_null<DeclRefExpr>(
        stripCasts(Access->getIdx()));
    if (!IndexRef || IndexRef->getDecl() != Index)
      return true;

    const auto *Storage = dyn_cast_or_null<MemberExpr>(
        stripCasts(Access->getBase()));
    if (Storage && Storage->getMemberDecl()->getName() ==
                       "secure_display_ctxs")
      Found = true;
    return true;
  }

  bool found() const { return Found; }
};

// Detect a loop that uses the number of display links as the bound for an
// indexed secure-display-context array.  The array is sized by the number of
// CRTCs, so the two counts are not interchangeable.
class SAGenTestChecker : public Checker<check::BranchCondition> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker() : BT(new BugType(this, "Secure Display Capacity Mismatch")) {}

  void checkBranchCondition(const Stmt *Condition, CheckerContext &C) const;
};

void SAGenTestChecker::checkBranchCondition(const Stmt *Condition,
                                            CheckerContext &C) const {
  const auto *CondExpr = dyn_cast<Expr>(Condition);
  const auto *Comparison = dyn_cast_or_null<BinaryOperator>(
      stripCasts(CondExpr));
  if (!Comparison || Comparison->getOpcode() != BO_LT ||
      !isMaxLinksCapability(Comparison->getRHS()))
    return;

  const auto *IndexRef = dyn_cast_or_null<DeclRefExpr>(
      stripCasts(Comparison->getLHS()));
  if (!IndexRef)
    return;

  const auto Parents = C.getASTContext().getParents(*Condition);
  if (Parents.empty())
    return;
  const auto *Loop = Parents[0].get<ForStmt>();
  if (!Loop)
    return;

  IndexedSecureDisplayContextFinder AccessFinder(IndexRef->getDecl());
  AccessFinder.TraverseStmt(const_cast<Stmt *>(Loop->getBody()));
  if (!AccessFinder.found())
    return;

  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT,
      "link capability count can exceed secure-display context capacity",
      N);
  Report->addRange(Condition->getSourceRange());
  C.emitReport(std::move(Report));
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker", 
      "Detects incorrect upper bound usage for secure display contexts", 
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] = 
    CLANG_ANALYZER_API_VERSION_STRING;
