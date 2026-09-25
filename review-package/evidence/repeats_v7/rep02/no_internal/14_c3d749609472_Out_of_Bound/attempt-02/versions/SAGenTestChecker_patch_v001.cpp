// /external/upstream-study/SAGEN/result-0224-bugfail-multi-o3mini/test-Out-of-Bound-c3d749609472ba0b217b42ab66f80459847e2bcb/checkers/checker1.cpp
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"

#include <memory>

using namespace clang;
using namespace ento;

namespace {

class LoopIndexUseVisitor
    : public RecursiveASTVisitor<LoopIndexUseVisitor> {
  const ValueDecl *Index;

public:
  bool UsesIndex = false;

  explicit LoopIndexUseVisitor(const ValueDecl *Index) : Index(Index) {}

  bool VisitArraySubscriptExpr(const ArraySubscriptExpr *Subscript) {
    const clang::Expr *SubscriptIndex =
        Subscript->getIdx()->IgnoreParenImpCasts();
    const auto *Reference = dyn_cast<DeclRefExpr>(SubscriptIndex);
    if (Reference && Reference->getDecl() == Index)
      UsesIndex = true;
    return true;
  }
};

static bool isDisplayLinkLimit(const clang::Expr *Bound) {
  Bound = Bound->IgnoreParenImpCasts();
  const auto *MaxLinks = dyn_cast<MemberExpr>(Bound);
  if (!MaxLinks || MaxLinks->getMemberDecl()->getName() != "max_links")
    return false;

  const auto *Caps = dyn_cast<MemberExpr>(
      MaxLinks->getBase()->IgnoreParenImpCasts());
  if (!Caps || Caps->getMemberDecl()->getName() != "caps")
    return false;

  const auto *DisplayCore = dyn_cast<MemberExpr>(
      Caps->getBase()->IgnoreParenImpCasts());
  return DisplayCore && DisplayCore->getMemberDecl()->getName() == "dc";
}

class SAGenTestChecker : public Checker<check::BranchCondition> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Mismatched Loop Bound and Indexed Capacity")) {}

  void checkBranchCondition(const Stmt *Condition, CheckerContext &C) const;
};

void SAGenTestChecker::checkBranchCondition(const Stmt *Condition,
                                             CheckerContext &C) const {
  const auto *CondExpr = dyn_cast<clang::Expr>(Condition);
  if (!CondExpr)
    return;

  const auto *Comparison =
      dyn_cast<BinaryOperator>(CondExpr->IgnoreParenImpCasts());
  if (!Comparison || Comparison->getOpcode() != BO_LT ||
      !isDisplayLinkLimit(Comparison->getRHS()))
    return;

  const auto *Index = dyn_cast<DeclRefExpr>(
      Comparison->getLHS()->IgnoreParenImpCasts());
  if (!Index)
    return;

  const ForStmt *Loop = nullptr;
  for (const DynTypedNode &Parent : C.getASTContext().getParents(*Condition)) {
    if ((Loop = Parent.get<ForStmt>()))
      break;
  }
  if (!Loop)
    return;

  LoopIndexUseVisitor Visitor(Index->getDecl());
  Visitor.TraverseStmt(const_cast<Stmt *>(Loop->getBody()));
  if (!Visitor.UsesIndex)
    return;

  ExplodedNode *Node = C.generateNonFatalErrorNode();
  if (!Node)
    return;

  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT, "Loop bound may exceed the capacity of an indexed array", Node);
  Report->addRange(Condition->getSourceRange());
  C.emitReport(std::move(Report));
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects unsafe display-link bounds on indexed arrays", "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
