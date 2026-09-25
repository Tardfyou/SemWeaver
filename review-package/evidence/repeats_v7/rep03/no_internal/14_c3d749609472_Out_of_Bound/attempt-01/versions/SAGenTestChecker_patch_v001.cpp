// /external/upstream-study/SAGEN/result-0224-bugfail-multi-o3mini/test-Out-of-Bound-c3d749609472ba0b217b42ab66f80459847e2bcb/checkers/checker1.cpp
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include "clang/AST/ParentMapContext.h"

using namespace clang;
using namespace ento;

namespace {

static bool isMaxLinksBound(const Expr *E) {
  E = E->IgnoreParenImpCasts();
  const auto *Member = dyn_cast<MemberExpr>(E);
  return Member && Member->getMemberDecl() &&
         Member->getMemberDecl()->getName() == "max_links";
}

static bool isSecureDisplayContexts(const Expr *E) {
  E = E->IgnoreParenImpCasts();
  if (const auto *Member = dyn_cast<MemberExpr>(E))
    return Member->getMemberDecl() &&
           Member->getMemberDecl()->getName() == "secure_display_ctxs";

  const auto *Reference = dyn_cast<DeclRefExpr>(E);
  return Reference && Reference->getDecl() &&
         Reference->getDecl()->getName() == "secure_display_ctxs";
}

static bool indexesSecureDisplayContexts(const Stmt *S,
                                         const VarDecl *Index) {
  if (!S)
    return false;

  if (const auto *Subscript = dyn_cast<ArraySubscriptExpr>(S)) {
    const Expr *SubscriptIndex =
        Subscript->getIdx()->IgnoreParenImpCasts();
    if (const auto *Reference = dyn_cast<DeclRefExpr>(SubscriptIndex)) {
      if (Reference->getDecl() == Index &&
          isSecureDisplayContexts(Subscript->getBase()))
        return true;
    }
  }

  for (const Stmt *Child : S->children()) {
    if (indexesSecureDisplayContexts(Child, Index))
      return true;
  }
  return false;
}

class SAGenTestChecker : public Checker<check::BranchCondition> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker() : BT(new BugType(this, "Incorrect Upper Bound Usage")) {}

  void checkBranchCondition(const Stmt *Condition, CheckerContext &C) const;
};

void SAGenTestChecker::checkBranchCondition(const Stmt *Condition,
                                            CheckerContext &C) const {
  const Expr *CondExpr = dyn_cast<Expr>(Condition);
  if (!CondExpr)
    return;

  const auto *Comparison =
      dyn_cast<BinaryOperator>(CondExpr->IgnoreParenImpCasts());
  if (!Comparison || (Comparison->getOpcode() != BO_LT &&
                      Comparison->getOpcode() != BO_LE) ||
      !isMaxLinksBound(Comparison->getRHS()))
    return;

  const auto *IndexReference =
      dyn_cast<DeclRefExpr>(Comparison->getLHS()->IgnoreParenImpCasts());
  const auto *Index = IndexReference
                          ? dyn_cast<VarDecl>(IndexReference->getDecl())
                          : nullptr;
  if (!Index)
    return;

  const auto Parents = C.getASTContext().getParents(*Condition);
  const auto *Loop = Parents.empty() ? nullptr : Parents[0].get<ForStmt>();
  if (!Loop || !indexesSecureDisplayContexts(Loop->getBody(), Index))
    return;

  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT, "Secure display contexts are indexed with a non-capacity bound", N);
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
