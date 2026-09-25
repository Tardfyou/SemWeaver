// /external/upstream-study/SAGEN/result-0224-bugfail-multi-o3mini/test-Double-Free-d8c561741ef83980114b3f7f95ffac54600f3f16/checkers/checker2.cpp
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
#include "clang/AST/Expr.h"  // for Expr

using namespace clang;
using namespace ento;
using namespace taint;

namespace {

class SAGenTestChecker : public Checker< check::PreCall > {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Double-free after failed SQ state transition"))
  {}

  void checkPreCall(const CallEvent &Call, CheckerContext &C) const;

private:
  static bool calleeNameIs(const CallExpr *CE, StringRef Name);
  static const VarDecl *getReferencedVar(const Expr *E);
  static bool stmtAssignsCallResultToVar(const Stmt *S, const VarDecl *VD,
                                         StringRef CalleeName);
  static bool stmtCallsFunction(const Stmt *S, StringRef CalleeName);
  static bool priorStatementsEstablishSqReadyFailure(const CompoundStmt *Block,
                                                     const IfStmt *If,
                                                     const VarDecl *ErrVar);
  static bool isSqReadyFailureCleanup(const Expr *OriginExpr,
                                      CheckerContext &C);
};

class VarRefFinder : public RecursiveASTVisitor<VarRefFinder> {
  const VarDecl *Found = nullptr;

public:
  bool VisitDeclRefExpr(const DeclRefExpr *DRE) {
    if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
      if (Found && Found != VD) {
        Found = nullptr;
        return false;
      }
      Found = VD;
    }
    return true;
  }

  const VarDecl *getFound() const { return Found; }
};

class CallNameFinder : public RecursiveASTVisitor<CallNameFinder> {
  StringRef TargetName;
  bool Found = false;

public:
  explicit CallNameFinder(StringRef TargetName) : TargetName(TargetName) {}

  bool VisitCallExpr(const CallExpr *CE) {
    if (SAGenTestChecker::calleeNameIs(CE, TargetName)) {
      Found = true;
      return false;
    }
    return true;
  }

  bool found() const { return Found; }
};

bool SAGenTestChecker::calleeNameIs(const CallExpr *CE, StringRef Name) {
  if (!CE)
    return false;

  const FunctionDecl *FD = CE->getDirectCallee();
  return FD && FD->getName() == Name;
}

const VarDecl *SAGenTestChecker::getReferencedVar(const Expr *E) {
  if (!E)
    return nullptr;

  VarRefFinder Finder;
  Finder.TraverseStmt(const_cast<Expr *>(E));
  return Finder.getFound();
}

bool SAGenTestChecker::stmtAssignsCallResultToVar(const Stmt *S,
                                                  const VarDecl *VD,
                                                  StringRef CalleeName) {
  if (!S || !VD)
    return false;

  if (const auto *BO = dyn_cast<BinaryOperator>(S->IgnoreImplicit())) {
    if (!BO->isAssignmentOp())
      return false;

    const auto *LHS = dyn_cast<DeclRefExpr>(BO->getLHS()->IgnoreParenImpCasts());
    const auto *RHS = dyn_cast<CallExpr>(BO->getRHS()->IgnoreParenImpCasts());
    return LHS && LHS->getDecl() == VD && calleeNameIs(RHS, CalleeName);
  }

  if (const auto *DS = dyn_cast<DeclStmt>(S)) {
    if (!DS->isSingleDecl())
      return false;

    const auto *DeclaredVar = dyn_cast<VarDecl>(DS->getSingleDecl());
    if (!DeclaredVar || DeclaredVar != VD || !DeclaredVar->hasInit())
      return false;

    const auto *InitCall = dyn_cast<CallExpr>(DeclaredVar->getInit()->IgnoreParenImpCasts());
    return calleeNameIs(InitCall, CalleeName);
  }

  return false;
}

bool SAGenTestChecker::stmtCallsFunction(const Stmt *S, StringRef CalleeName) {
  if (!S)
    return false;

  CallNameFinder Finder(CalleeName);
  Finder.TraverseStmt(const_cast<Stmt *>(S));
  return Finder.found();
}

bool SAGenTestChecker::priorStatementsEstablishSqReadyFailure(
    const CompoundStmt *Block, const IfStmt *If, const VarDecl *ErrVar) {
  if (!Block || !If || !ErrVar)
    return false;

  bool SawCreateSq = false;
  bool SawReadyTransition = false;
  for (const Stmt *Child : Block->body()) {
    if (Child == If)
      break;

    SawCreateSq |= stmtCallsFunction(Child, "hws_send_ring_create_sq");
    if (stmtAssignsCallResultToVar(Child, ErrVar, "hws_send_ring_set_sq_rdy"))
      SawReadyTransition = true;
  }

  return SawCreateSq && SawReadyTransition;
}

bool SAGenTestChecker::isSqReadyFailureCleanup(const Expr *OriginExpr,
                                               CheckerContext &C) {
  const auto *CE = dyn_cast_or_null<CallExpr>(OriginExpr);
  if (!calleeNameIs(CE, "hws_send_ring_close_sq"))
    return false;

  const IfStmt *If = findSpecificTypeInParents<IfStmt>(OriginExpr, C);
  if (!If || !If->getCond())
    return false;

  const VarDecl *ErrVar = getReferencedVar(If->getCond());
  if (!ErrVar)
    return false;

  const CompoundStmt *Block = findSpecificTypeInParents<CompoundStmt>(If, C);
  return priorStatementsEstablishSqReadyFailure(Block, If, ErrVar);
}

void SAGenTestChecker::checkPreCall(const CallEvent &Call, CheckerContext &C) const {
  const Expr *OriginExpr = Call.getOriginExpr();
  if (!OriginExpr)
    return;

  if (!isSqReadyFailureCleanup(OriginExpr->IgnoreParenImpCasts(), C))
    return;

  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT, "Double-free risk: full SQ close is used after SQ ready transition fails", N);
  Report->addRange(Call.getSourceRange());
  C.emitReport(std::move(Report));
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker", 
      "Detects error path cleanup using wrong routine 'hws_send_ring_close_sq'", 
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
