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
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtVisitor.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/StaticAnalyzer/Checkers/utility.h"
#include "clang/Lex/Lexer.h"  // Needed for Lexer utilities

using namespace clang;
using namespace ento;
using namespace taint;

namespace {

class SAGenTestChecker : public Checker<check::PreStmt<ForStmt>> {
  mutable std::unique_ptr<BugType> BT;

  static const Expr *stripCasts(const Expr *E) {
    return E ? E->IgnoreParenImpCasts() : nullptr;
  }

  static bool hasFieldName(const MemberExpr *E, llvm::StringRef Name) {
    const auto *Field = E ? dyn_cast<FieldDecl>(E->getMemberDecl()) : nullptr;
    return Field && Field->getName() == Name;
  }

  static const VarDecl *referencedVar(const Expr *E) {
    const auto *Ref = dyn_cast_or_null<DeclRefExpr>(stripCasts(E));
    return Ref ? dyn_cast<VarDecl>(Ref->getDecl()) : nullptr;
  }

  static bool isLinkCapacity(const Expr *E) {
    const auto *MaxLinks = dyn_cast_or_null<MemberExpr>(stripCasts(E));
    if (!hasFieldName(MaxLinks, "max_links"))
      return false;

    const auto *Caps = dyn_cast_or_null<MemberExpr>(
        stripCasts(MaxLinks->getBase()));
    return hasFieldName(Caps, "caps");
  }

  static bool isSecureDisplayContextArray(const Expr *E) {
    const auto *Member = dyn_cast_or_null<MemberExpr>(stripCasts(E));
    return hasFieldName(Member, "secure_display_ctxs");
  }

  class IndexedContextVisitor
      : public RecursiveASTVisitor<IndexedContextVisitor> {
    const VarDecl *Index;

  public:
    bool Found = false;

    explicit IndexedContextVisitor(const VarDecl *Index) : Index(Index) {}

    bool VisitArraySubscriptExpr(ArraySubscriptExpr *Subscript) {
      if (referencedVar(Subscript->getIdx()) == Index &&
          isSecureDisplayContextArray(Subscript->getBase()))
        Found = true;
      return !Found;
    }
  };

public:
  SAGenTestChecker() : BT(new BugType(this, "Incorrect Upper Bound Usage")) {}

  void checkPreStmt(const ForStmt *Loop, CheckerContext &C) const;
};

void SAGenTestChecker::checkPreStmt(const ForStmt *Loop,
                                    CheckerContext &C) const {
  const auto *Condition = dyn_cast_or_null<BinaryOperator>(
      stripCasts(Loop->getCond()));
  if (!Condition || Condition->getOpcode() != BO_LT ||
      !isLinkCapacity(Condition->getRHS()))
    return;

  const VarDecl *Index = referencedVar(Condition->getLHS());
  if (!Index || !Loop->getBody())
    return;

  IndexedContextVisitor Visitor(Index);
  Visitor.TraverseStmt(const_cast<Stmt *>(Loop->getBody()));
  if (!Visitor.Found)
    return;

  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT,
      "Link-count loop bound indexes CRTC secure-display context slots",
      N);
  Report->addRange(Loop->getCond()->getSourceRange());
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
