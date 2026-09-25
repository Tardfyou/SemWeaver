```cpp
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

#include <memory>

using namespace clang;
using namespace ento;
using namespace taint;

REGISTER_SET_WITH_PROGRAMSTATE(ReportedSecureDisplayLoops, const Stmt *)

namespace {

static bool isNamedMember(const Expr *E, llvm::StringRef Name) {
  if (!E)
    return false;

  E = E->IgnoreParenImpCasts();
  const auto *ME = dyn_cast<MemberExpr>(E);
  if (!ME)
    return false;

  const ValueDecl *Member = ME->getMemberDecl();
  return Member && Member->getName() == Name;
}

static bool isMaxLinksExpr(const Expr *E) {
  if (!E)
    return false;

  E = E->IgnoreParenImpCasts();
  const auto *MaxLinks = dyn_cast<MemberExpr>(E);
  if (!MaxLinks || !MaxLinks->getMemberDecl() ||
      MaxLinks->getMemberDecl()->getName() != "max_links")
    return false;

  // Match dc->caps.max_links structurally, rather than by source text.
  return isNamedMember(MaxLinks->getBase(), "caps");
}

static const VarDecl *getReferencedVar(const Expr *E) {
  if (!E)
    return nullptr;

  E = E->IgnoreParenImpCasts();
  const auto *DRE = dyn_cast<DeclRefExpr>(E);
  if (!DRE)
    return nullptr;

  return dyn_cast<VarDecl>(DRE->getDecl());
}

static bool getMaxLinksLoopIndex(const Expr *Condition,
                                 const VarDecl *&LoopIndex) {
  LoopIndex = nullptr;
  if (!Condition)
    return false;

  Condition = Condition->IgnoreParenImpCasts();
  const auto *BO = dyn_cast<BinaryOperator>(Condition);
  if (!BO)
    return false;

  const Expr *IndexExpr = nullptr;
  const Expr *BoundExpr = nullptr;

  switch (BO->getOpcode()) {
  case BO_LT:
  case BO_LE:
    IndexExpr = BO->getLHS();
    BoundExpr = BO->getRHS();
    break;
  case BO_GT:
  case BO_GE:
    IndexExpr = BO->getRHS();
    BoundExpr = BO->getLHS();
    break;
  default:
    return false;
  }

  if (!isMaxLinksExpr(BoundExpr))
    return false;

  LoopIndex = getReferencedVar(IndexExpr);
  return LoopIndex != nullptr;
}

static bool isSecureDisplayContextsBase(const Expr *Base) {
  if (!Base)
    return false;

  Base = Base->IgnoreParenImpCasts();

  // Covers adev->dm.secure_display_ctxs[i].
  if (const auto *ME = dyn_cast<MemberExpr>(Base)) {
    const ValueDecl *Member = ME->getMemberDecl();
    return Member && Member->getName() == "secure_display_ctxs";
  }

  // Covers the local secure_display_ctxs[i] used during allocation setup.
  if (const auto *DRE = dyn_cast<DeclRefExpr>(Base)) {
    const auto *VD = dyn_cast<VarDecl>(DRE->getDecl());
    return VD && VD->getName() == "secure_display_ctxs";
  }

  return false;
}

class LoopIndexReferenceVisitor
    : public RecursiveASTVisitor<LoopIndexReferenceVisitor> {
  const VarDecl *LoopIndex;
  bool Found = false;

public:
  explicit LoopIndexReferenceVisitor(const VarDecl *LoopIndex)
      : LoopIndex(LoopIndex) {}

  bool VisitDeclRefExpr(const DeclRefExpr *DRE) {
    if (DRE->getDecl() == LoopIndex)
      Found = true;

    return !Found;
  }

  bool found() const {
    return Found;
  }
};

static bool referencesLoopIndex(const Expr *E, const VarDecl *LoopIndex) {
  if (!E || !LoopIndex)
    return false;

  LoopIndexReferenceVisitor Visitor(LoopIndex);
  Visitor.TraverseStmt(const_cast<Expr *>(E));
  return Visitor.found();
}

class SecureDisplayAccessVisitor
    : public RecursiveASTVisitor<SecureDisplayAccessVisitor> {
  const VarDecl *LoopIndex;
  bool FoundUnsafeAccess = false;

public:
  explicit SecureDisplayAccessVisitor(const VarDecl *LoopIndex)
      : LoopIndex(LoopIndex) {}

  bool VisitArraySubscriptExpr(const ArraySubscriptExpr *ASE) {
    if (!isSecureDisplayContextsBase(ASE->getBase()))
      return true;

    // i, i + 1, and equivalent expressions which reference the loop's
    // induction variable are all controlled by the max_links loop bound.
    if (referencesLoopIndex(ASE->getIdx(), LoopIndex))
      FoundUnsafeAccess = true;

    return !FoundUnsafeAccess;
  }

  bool foundUnsafeAccess() const {
    return FoundUnsafeAccess;
  }
};

static bool loopBodyIndexesSecureDisplayContexts(const Stmt *LoopBody,
                                                 const VarDecl *LoopIndex) {
  if (!LoopBody || !LoopIndex)
    return false;

  SecureDisplayAccessVisitor Visitor(LoopIndex);
  Visitor.TraverseStmt(const_cast<Stmt *>(LoopBody));
  return Visitor.foundUnsafeAccess();
}

static const Stmt *getLoopBodyForCondition(const Stmt *Condition,
                                           CheckerContext &C) {
  if (!Condition)
    return nullptr;

  for (const DynTypedNode &Parent :
       C.getASTContext().getParents(*Condition)) {
    if (const auto *FS = Parent.get<ForStmt>()) {
      if (FS->getCond() == Condition)
        return FS->getBody();
    }

    if (const auto *WS = Parent.get<WhileStmt>()) {
      if (WS->getCond() == Condition)
        return WS->getBody();
    }

    if (const auto *DS = Parent.get<DoStmt>()) {
      if (DS->getCond() == Condition)
        return DS->getBody();
    }
  }

  return nullptr;
}

// This checker detects max_links loop bounds only when they control indexing
// of the CRTC-indexed secure_display_ctxs array.
class SAGenTestChecker : public Checker<check::BranchCondition> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Incorrect Upper Bound Usage")) {}

  void checkBranchCondition(const Stmt *Condition,
                            CheckerContext &C) const;
};

void SAGenTestChecker::checkBranchCondition(const Stmt *Condition,
                                            CheckerContext &C) const {
  const auto *CondExpr = dyn_cast<Expr>(Condition);
  if (!CondExpr)
    return;

  const VarDecl *LoopIndex = nullptr;
  if (!getMaxLinksLoopIndex(CondExpr, LoopIndex))
    return;

  const Stmt *LoopBody = getLoopBodyForCondition(Condition, C);
  if (!LoopBody)
    return;

  // max_links is valid for per-link resources, including hpd_rx_offload_wq.
  // Only secure_display_ctxs has the CRTC-count ownership contract targeted
  // by this checker.
  if (!loopBodyIndexesSecureDisplayContexts(LoopBody, LoopIndex))
    return;

  ProgramStateRef State = C.getState();
  if (State->contains<ReportedSecureDisplayLoops>(Condition))
    return;

  State = State->add<ReportedSecureDisplayLoops>(Condition);

  ExplodedNode *N = C.generateNonFatalErrorNode(State);
  if (!N)
    return;

  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT,
      "Incorrect upper bound: secure_display_ctxs is CRTC-indexed; "
      "use mode_info.num_crtc instead of dc->caps.max_links",
      N);
  Report->addRange(Condition->getSourceRange());
  C.emitReport(std::move(Report));
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects max_links bounds used to index secure display contexts",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
```