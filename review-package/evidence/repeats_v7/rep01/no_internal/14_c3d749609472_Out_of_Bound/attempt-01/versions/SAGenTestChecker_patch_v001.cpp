#include "clang/AST/Expr.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"

#include <memory>

using namespace clang;
using namespace ento;

namespace {

static const VarDecl *getReferencedVariable(const Expr *E) {
  E = E->IgnoreParenImpCasts();
  const auto *DRE = dyn_cast<DeclRefExpr>(E);
  return DRE ? dyn_cast<VarDecl>(DRE->getDecl()) : nullptr;
}

static const VarDecl *getRootVariable(const Expr *E) {
  E = E->IgnoreParenImpCasts();
  while (const auto *ME = dyn_cast<MemberExpr>(E))
    E = ME->getBase()->IgnoreParenImpCasts();
  return getReferencedVariable(E);
}

static const VarDecl *getMaxLinksOwner(const Expr *E) {
  E = E->IgnoreParenImpCasts();
  const auto *ME = dyn_cast<MemberExpr>(E);
  if (!ME || ME->getMemberDecl()->getNameAsString() != "max_links")
    return nullptr;
  return getRootVariable(ME->getBase());
}

class SecureDisplayElementVisitor
    : public RecursiveASTVisitor<SecureDisplayElementVisitor> {
  const VarDecl *Index;
  const VarDecl *Owner;
  bool Found = false;

public:
  SecureDisplayElementVisitor(const VarDecl *Index, const VarDecl *Owner)
      : Index(Index), Owner(Owner) {}

  bool VisitArraySubscriptExpr(ArraySubscriptExpr *ASE) {
    if (getReferencedVariable(ASE->getIdx()) != Index)
      return true;

    const Expr *Base = ASE->getBase()->IgnoreParenImpCasts();
    const auto *ME = dyn_cast<MemberExpr>(Base);
    if (ME && ME->getMemberDecl()->getNameAsString() ==
                  "secure_display_ctxs" &&
        getRootVariable(ME->getBase()) == Owner)
      Found = true;
    return true;
  }

  bool found() const { return Found; }
};

static bool hasUnsafeSecureDisplayBound(const ForStmt *FS) {
  const Expr *Condition = FS->getCond();
  if (!Condition)
    return false;

  const auto *Comparison =
      dyn_cast<BinaryOperator>(Condition->IgnoreParenImpCasts());
  if (!Comparison || Comparison->getOpcode() != BO_LT)
    return false;

  const VarDecl *Index = getReferencedVariable(Comparison->getLHS());
  const VarDecl *Owner = getMaxLinksOwner(Comparison->getRHS());
  if (!Index || !Owner || !FS->getBody())
    return false;

  SecureDisplayElementVisitor Visitor(Index, Owner);
  Visitor.TraverseStmt(const_cast<Stmt *>(FS->getBody()));
  return Visitor.found();
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

  for (const DynTypedNode &Parent : C.getASTContext().getParents(*Condition)) {
    const auto *FS = Parent.get<ForStmt>();
    if (!FS || FS->getCond() != CondExpr || !hasUnsafeSecureDisplayBound(FS))
      continue;

    ExplodedNode *N = C.generateNonFatalErrorNode();
    if (!N)
      return;

    auto Report = std::make_unique<PathSensitiveBugReport>(
        *BT,
        "secure_display_ctxs is indexed using max_links rather than its crtc capacity",
        N);
    Report->addRange(Condition->getSourceRange());
    C.emitReport(std::move(Report));
    return;
  }
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects secure-display loops bounded by a non-capacity link count", "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
