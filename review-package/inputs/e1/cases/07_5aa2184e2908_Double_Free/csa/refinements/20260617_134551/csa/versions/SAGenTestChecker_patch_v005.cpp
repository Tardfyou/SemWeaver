// /external/upstream-study/SAGEN/result-0224-bugfail-multi-o3mini/test-Double-Free-5aa2184e29081665f915594bc6de9b7fee6e4883/checkers/checker0.cpp
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
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"

using namespace clang;
using namespace ento;
using namespace taint;

namespace {

static const Expr *stripExpr(const Expr *E) {
  return E ? E->IgnoreParenImpCasts() : nullptr;
}

static const DeclRefExpr *getDeclRef(const Expr *E) {
  return dyn_cast_or_null<DeclRefExpr>(stripExpr(E));
}

static const DeclRefExpr *getMemberBaseDecl(const Expr *E) {
  const auto *ME = dyn_cast_or_null<MemberExpr>(stripExpr(E));
  if (!ME)
    return nullptr;
  return getDeclRef(ME->getBase());
}

static bool stmtContainsDeclRef(const Stmt *S, const ValueDecl *VD) {
  if (!S || !VD)
    return false;
  if (const auto *DRE = dyn_cast<DeclRefExpr>(S))
    return DRE->getDecl() == VD;
  for (const Stmt *Child : S->children())
    if (stmtContainsDeclRef(Child, VD))
      return true;
  return false;
}

static bool stmtContainsGotoTo(const Stmt *S, const LabelDecl *Target) {
  if (!S || !Target)
    return false;
  if (const auto *GS = dyn_cast<GotoStmt>(S))
    return GS->getLabel() == Target;
  for (const Stmt *Child : S->children())
    if (stmtContainsGotoTo(Child, Target))
      return true;
  return false;
}

class ErrorCleanupAnalyzer {
  struct LastFallibleInit {
    const ValueDecl *RetDecl = nullptr;
    const ValueDecl *OwnerDecl = nullptr;
    bool Valid = false;
  };

  const LabelDecl *CleanupLabel;
  const ValueDecl *FreedOwner;

  static bool callMentionsOwner(const CallExpr *CE, const ValueDecl *Owner) {
    if (!CE || !Owner)
      return false;
    for (const Expr *Arg : CE->arguments()) {
      if (const auto *DRE = getDeclRef(Arg))
        if (DRE->getDecl() == Owner)
          return true;
    }
    return false;
  }

  static LastFallibleInit getFallibleInit(const Stmt *S,
                                          const ValueDecl *FreedOwner) {
    LastFallibleInit Result;
    const Expr *RHS = nullptr;
    const ValueDecl *RetDecl = nullptr;

    if (const auto *BO = dyn_cast<BinaryOperator>(S)) {
      if (BO->isAssignmentOp()) {
        if (const auto *DRE = getDeclRef(BO->getLHS())) {
          RetDecl = DRE->getDecl();
          RHS = BO->getRHS();
        }
      }
    } else if (const auto *DS = dyn_cast<DeclStmt>(S)) {
      if (DS->isSingleDecl()) {
        if (const auto *VD = dyn_cast<VarDecl>(DS->getSingleDecl())) {
          RetDecl = VD;
          RHS = VD->getInit();
        }
      }
    }

    const auto *CE = dyn_cast_or_null<CallExpr>(stripExpr(RHS));
    if (!CE || !RetDecl || !callMentionsOwner(CE, FreedOwner))
      return Result;

    Result.RetDecl = RetDecl;
    Result.OwnerDecl = FreedOwner;
    Result.Valid = true;
    return Result;
  }

  bool analyzeStmt(const Stmt *S, LastFallibleInit Last) const {
    if (!S)
      return false;

    if (const auto *CS = dyn_cast<CompoundStmt>(S)) {
      LastFallibleInit Current = Last;
      for (const Stmt *Child : CS->body()) {
        LastFallibleInit NewInit = getFallibleInit(Child, FreedOwner);
        if (NewInit.Valid) {
          Current = NewInit;
          continue;
        }
        if (isa<BinaryOperator>(Child) || isa<DeclStmt>(Child))
          Current.Valid = false;
        if (analyzeStmt(Child, Current))
          return true;
      }
      return false;
    }

    if (const auto *IfS = dyn_cast<IfStmt>(S)) {
      if (Last.Valid && Last.OwnerDecl == FreedOwner &&
          stmtContainsDeclRef(IfS->getCond(), Last.RetDecl) &&
          stmtContainsGotoTo(IfS->getThen(), CleanupLabel))
        return true;
    }

    for (const Stmt *Child : S->children())
      if (analyzeStmt(Child, Last))
        return true;
    return false;
  }

public:
  ErrorCleanupAnalyzer(const LabelDecl *CleanupLabel,
                       const ValueDecl *FreedOwner)
      : CleanupLabel(CleanupLabel), FreedOwner(FreedOwner) {}

  bool hasFailedInitCleanupPath(const Stmt *Body) const {
    return analyzeStmt(Body, LastFallibleInit());
  }
};

class SAGenTestChecker : public Checker<check::PreCall> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker() : BT(new BugType(this, "Invalid cleanup after failed initialization")) {}

  void checkPreCall(const CallEvent &Call, CheckerContext &C) const;

private:
  void reportInvalidCleanup(const CallEvent &Call, CheckerContext &C) const;
};

void SAGenTestChecker::checkPreCall(const CallEvent &Call, CheckerContext &C) const {
  const Expr *OriginExpr = Call.getOriginExpr();
  if (!OriginExpr || !ExprHasName(OriginExpr, "kfree", C) || Call.getNumArgs() < 1)
    return;

  const Expr *FreedExpr = Call.getArgExpr(0);
  const auto *FreedMember = dyn_cast_or_null<MemberExpr>(stripExpr(FreedExpr));
  if (!FreedMember)
    return;

  const DeclRefExpr *OwnerRef = getMemberBaseDecl(FreedExpr);
  if (!OwnerRef)
    return;

  const auto *CleanupLabel = findSpecificTypeInParents<LabelStmt>(OriginExpr, C);
  if (!CleanupLabel)
    return;

  const Decl *D = C.getLocationContext()->getDecl();
  const auto *FD = dyn_cast_or_null<FunctionDecl>(D);
  if (!FD || !FD->hasBody())
    return;

  ErrorCleanupAnalyzer Analyzer(CleanupLabel->getDecl(), OwnerRef->getDecl());
  if (!Analyzer.hasFailedInitCleanupPath(FD->getBody()))
    return;

  reportInvalidCleanup(Call, C);
}

void SAGenTestChecker::reportInvalidCleanup(const CallEvent &Call,
                                            CheckerContext &C) const {
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;
  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT, "Cleanup frees an owned member on a failure path before the initializer has established that member", N);
  Report->addRange(Call.getSourceRange());
  C.emitReport(std::move(Report));
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker", 
      "Detects cleanup paths that free unallocated resources, which may indicate a double free error",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
