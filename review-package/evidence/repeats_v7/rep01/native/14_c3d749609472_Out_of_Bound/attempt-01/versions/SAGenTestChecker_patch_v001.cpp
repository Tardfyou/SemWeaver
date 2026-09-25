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

static const VarDecl *getLoopIndex(const Expr *Condition) {
  const auto *Comparison = dyn_cast<BinaryOperator>(
      Condition->IgnoreParenImpCasts());
  if (!Comparison || Comparison->getOpcode() != BO_LT)
    return nullptr;

  const auto *Index = dyn_cast<DeclRefExpr>(
      Comparison->getLHS()->IgnoreParenImpCasts());
  return Index ? dyn_cast<VarDecl>(Index->getDecl()) : nullptr;
}

static bool isMaxLinksBound(const Expr *Condition) {
  const auto *Comparison = dyn_cast<BinaryOperator>(
      Condition->IgnoreParenImpCasts());
  if (!Comparison || Comparison->getOpcode() != BO_LT)
    return false;

  const auto *Bound = dyn_cast<MemberExpr>(
      Comparison->getRHS()->IgnoreParenImpCasts());
  if (!Bound || Bound->getMemberNameInfo().getAsString() != "max_links")
    return false;

  const auto *Caps = dyn_cast<MemberExpr>(
      Bound->getBase()->IgnoreParenImpCasts());
  return Caps && Caps->getMemberNameInfo().getAsString() == "caps";
}

class SecureContextIndexVisitor
    : public RecursiveASTVisitor<SecureContextIndexVisitor> {
  const VarDecl *Index;

public:
  bool UsesSecureContext = false;

  explicit SecureContextIndexVisitor(const VarDecl *Index) : Index(Index) {}

  bool VisitArraySubscriptExpr(ArraySubscriptExpr *Subscript) {
    const auto *SubscriptIndex = dyn_cast<DeclRefExpr>(
        Subscript->getIdx()->IgnoreParenImpCasts());
    if (!SubscriptIndex || SubscriptIndex->getDecl()->getCanonicalDecl() !=
                               Index->getCanonicalDecl())
      return true;

    const auto *Base = dyn_cast<MemberExpr>(
        Subscript->getBase()->IgnoreParenImpCasts());
    if (Base && Base->getMemberNameInfo().getAsString() ==
                    "secure_display_ctxs")
      UsesSecureContext = true;
    return true;
  }
};

static const ForStmt *getContainingLoop(const Stmt *Condition,
                                        ASTContext &Context) {
  const Stmt *Current = Condition;
  while (Current) {
    auto Parents = Context.getParents(*Current);
    if (Parents.empty())
      return nullptr;
    if (const auto *Loop = Parents[0].get<ForStmt>())
      return Loop;
    Current = Parents[0].get<Stmt>();
  }
  return nullptr;
}

// Detect loops that index secure-display contexts using the link capability
// rather than the context capacity established from the CRTC count.
class SAGenTestChecker : public Checker<check::BranchCondition> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker() : BT(new BugType(this, "Incorrect Upper Bound Usage")) {}

  void checkBranchCondition(const Stmt *Condition, CheckerContext &C) const;
};

void SAGenTestChecker::checkBranchCondition(const Stmt *Condition,
                                            CheckerContext &C) const {
  const Expr *CondExpr = dyn_cast<Expr>(Condition);
  if (!CondExpr || !isMaxLinksBound(CondExpr))
    return;

  const VarDecl *Index = getLoopIndex(CondExpr);
  const ForStmt *Loop = getContainingLoop(Condition, C.getASTContext());
  if (!Index || !Loop || !Loop->getBody())
    return;

  SecureContextIndexVisitor Visitor(Index);
  Visitor.TraverseStmt(const_cast<Stmt *>(Loop->getBody()));
  if (!Visitor.UsesSecureContext)
    return;

  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT,
      "Secure display context array is indexed with a link-capability bound",
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
