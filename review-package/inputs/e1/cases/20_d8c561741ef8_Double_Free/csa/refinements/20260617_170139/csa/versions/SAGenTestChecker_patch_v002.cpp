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
  static const CallExpr *getAssignedCallToVar(const Stmt *S, const VarDecl *VD,
                                              StringRef CalleeName);
  static bool stmtAssignsCallResultToVar(const Stmt *S, const VarDecl *VD,
                                         StringRef CalleeName);
  static bool stmtCallsFunction(const Stmt *S, StringRef CalleeName);
  static bool callHasVarArg(const CallExpr *CE, const VarDecl *VD);
  static bool priorStatementsEstablishSqReadyFailure(const CompoundStmt *Block,
                                                     const IfStmt *If,
                                                     const VarDecl *ErrVar,
                                                     const VarDecl *SqVar,
                                                     const VarDecl *MdevVar);
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

const CallExpr *SAGenTestChecker::getAssignedCallToVar(const Stmt *S,
                                                       const VarDecl *VD,
                                                       StringRef CalleeName) {
  if (!S || !VD)
    return nullptr;

  if (const auto *BO = dyn_cast<BinaryOperator>(S->IgnoreImplicit())) {
    if (!BO->isAssignmentOp())
      return nullptr;

    const auto *LHS = dyn_cast<DeclRefExpr>(BO->getLHS()->IgnoreParenImpCasts());
    const auto *RHS = dyn_cast<CallExpr>(BO->getRHS()->IgnoreParenImpCasts());
    return LHS && LHS->getDecl() == VD && calleeNameIs(RHS, CalleeName) ? RHS : nullptr;
  }

  if (const auto *DS = dyn_cast<DeclStmt>(S)) {
    if (!DS->isSingleDecl())
      return nullptr;

    const auto *DeclaredVar = dyn_cast<VarDecl>(DS->getSingleDecl());
    if (!DeclaredVar || DeclaredVar != VD || !DeclaredVar->hasInit())
      return nullptr;

    const auto *InitCall = dyn_cast<CallExpr>(DeclaredVar->getInit()->IgnoreParenImpCasts());
    return calleeNameIs(InitCall, CalleeName) ? InitCall : nullptr;
  }

  return nullptr;
}

bool SAGenTestChecker::stmtAssignsCallResultToVar(const Stmt *S,
                                                  const VarDecl *VD,
                                                  StringRef CalleeName) {
  return getAssignedCallToVar(S, VD, CalleeName) != nullptr;
}

bool SAGenTestChecker::stmtCallsFunction(const Stmt *S, StringRef CalleeName) {
  if (!S)
    return false;

  CallNameFinder Finder(CalleeName);
  Finder.TraverseStmt(const_cast<Stmt *>(S));
  return Finder.found();
}

bool SAGenTestChecker::callHasVarArg(const CallExpr *CE, const VarDecl *VD) {
  if (!CE || !VD)
    return false;

  for (const Expr *Arg : CE->arguments()) {
    if (getReferencedVar(Arg) == VD)
      return true;
  }
  return false;
}

bool SAGenTestChecker::priorStatementsEstablishSqReadyFailure(
    const CompoundStmt *Block, const IfStmt *If, const VarDecl *ErrVar,
    const VarDecl *SqVar, const VarDecl *MdevVar) {
  if (!Block || !If || !ErrVar || !SqVar)
    return false;

  bool SawCreateSqForObject = false;
  bool SawReadyTransitionForObject = false;
  for (const Stmt *Child : Block->body()) {
    if (Child == If)
      break;

    if (const auto *CE = dyn_cast<CallExpr>(Child->IgnoreImplicit())) {
      if (calleeNameIs(CE, "hws_send_ring_create_sq") && callHasVarArg(CE, SqVar) &&
          (!MdevVar || callHasVarArg(CE, MdevVar)))
        SawCreateSqForObject = true;
    }

    if (const CallExpr *ReadyCall = getAssignedCallToVar(Child, ErrVar,
                                                         "hws_send_ring_set_sq_rdy")) {
      const VarDecl *ReadyMdevVar = ReadyCall->getNumArgs() > 0 ?
          getReferencedVar(ReadyCall->getArg(0)) : nullptr;
      const VarDecl *ReadySqVar = ReadyCall->getNumArgs() > 1 ?
          getReferencedVar(ReadyCall->getArg(1)) : nullptr;
      if ((!MdevVar || ReadyMdevVar == MdevVar) && ReadySqVar == SqVar)
        SawReadyTransitionForObject = true;
    }
  }

  return SawCreateSqForObject && SawReadyTransitionForObject;
}

bool SAGenTestChecker::isSqReadyFailureCleanup(const Expr *OriginExpr,
                                               CheckerContext &C) {
  const auto *CE = dyn_cast_or_null<CallExpr>(OriginExpr);
  if (!calleeNameIs(CE, "hws_send_ring_close_sq"))
    return false;

  if (CE->getNumArgs() == 0)
    return false;

  const VarDecl *SqVar = getReferencedVar(CE->getArg(0));
  if (!SqVar)
    return false;

  const IfStmt *If = findSpecificTypeInParents<IfStmt>(OriginExpr, C);
  if (!If || !If->getCond())
    return false;

  const VarDecl *ErrVar = getReferencedVar(If->getCond());
  if (!ErrVar)
    return false;

  const CompoundStmt *Block = findSpecificTypeInParents<CompoundStmt>(If, C);
  const VarDecl *MdevVar = nullptr;
  if (Block) {
    for (const Stmt *Child : Block->body()) {
      if (Child == If)
        break;

      if (const CallExpr *ReadyCall = getAssignedCallToVar(Child, ErrVar,
                                                           "hws_send_ring_set_sq_rdy")) {
        if (ReadyCall->getNumArgs() > 0)
          MdevVar = getReferencedVar(ReadyCall->getArg(0));
        break;
      }
    }
  }

  return priorStatementsEstablishSqReadyFailure(Block, If, ErrVar, SqVar, MdevVar);
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
