// /external/upstream-study/SAGEN/result-0224-bugfail-multi-o3mini/test-Double-Free-d8c561741ef83980114b3f7f95ffac54600f3f16/checkers/checker2.cpp
#include "clang/AST/Expr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtVisitor.h"
#include "clang/StaticAnalyzer/Checkers/Taint.h"
#include "clang/StaticAnalyzer/Checkers/utility.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;
using namespace ento;
using namespace taint;

namespace {

class SAGenTestChecker : public Checker<check::PreCall> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Double-free after failed SQ ready transition")) {}

  void checkPreCall(const CallEvent &Call, CheckerContext &C) const;

private:
  bool isFailedReadyCleanupClose(const CallEvent &Call, CheckerContext &C) const;
};

static const Expr *ignoreParenCasts(const Expr *E) {
  return E ? E->IgnoreParenImpCasts() : nullptr;
}

static const CallExpr *asCallExpr(const Stmt *S) {
  if (const auto *E = dyn_cast_or_null<Expr>(S))
    S = ignoreParenCasts(E);
  return dyn_cast_or_null<CallExpr>(S);
}

static bool calleeIs(const CallExpr *CE, StringRef Name) {
  const FunctionDecl *FD = CE ? CE->getDirectCallee() : nullptr;
  return FD && FD->getIdentifier() && FD->getName() == Name;
}

static bool stmtContains(const Stmt *Root, const Stmt *Needle) {
  if (!Root || !Needle)
    return false;
  if (Root == Needle)
    return true;
  for (const Stmt *Child : Root->children())
    if (stmtContains(Child, Needle))
      return true;
  return false;
}

static const CallExpr *findCallNamed(const Stmt *Root, StringRef Name) {
  if (!Root)
    return nullptr;
  if (const CallExpr *CE = asCallExpr(Root))
    if (calleeIs(CE, Name))
      return CE;
  for (const Stmt *Child : Root->children())
    if (const CallExpr *Found = findCallNamed(Child, Name))
      return Found;
  return nullptr;
}

static const VarDecl *declRefVar(const Expr *E) {
  E = ignoreParenCasts(E);
  if (const auto *DRE = dyn_cast_or_null<DeclRefExpr>(E))
    return dyn_cast_or_null<VarDecl>(DRE->getDecl());
  return nullptr;
}

static const VarDecl *baseVar(const Expr *E) {
  E = ignoreParenCasts(E);
  if (const VarDecl *VD = declRefVar(E))
    return VD;
  if (const auto *ME = dyn_cast_or_null<MemberExpr>(E))
    return baseVar(ME->getBase());
  if (const auto *UO = dyn_cast_or_null<UnaryOperator>(E))
    return baseVar(UO->getSubExpr());
  return nullptr;
}

static void collectReferencedVars(const Stmt *Root,
                                  llvm::SmallPtrSetImpl<const VarDecl *> &Vars) {
  if (!Root)
    return;
  if (const auto *DRE = dyn_cast<DeclRefExpr>(Root))
    if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl()))
      Vars.insert(VD);
  for (const Stmt *Child : Root->children())
    collectReferencedVars(Child, Vars);
}

static const VarDecl *assignedVarFromCall(const Stmt *S, StringRef CalleeName) {
  if (const auto *BO = dyn_cast_or_null<BinaryOperator>(S))
    if (BO->isAssignmentOp() && findCallNamed(BO->getRHS(), CalleeName))
      return declRefVar(BO->getLHS());

  if (const auto *DS = dyn_cast_or_null<DeclStmt>(S))
    if (DS->isSingleDecl())
      if (const auto *VD = dyn_cast<VarDecl>(DS->getSingleDecl()))
        if (VD->getInit() && findCallNamed(VD->getInit(), CalleeName))
          return VD;

  if (const auto *E = dyn_cast_or_null<Expr>(S))
    return assignedVarFromCall(ignoreParenCasts(E), CalleeName);

  if (const auto *ES = dyn_cast_or_null<ExprWithCleanups>(S))
    return assignedVarFromCall(ES->getSubExpr(), CalleeName);

  return nullptr;
}

static const VarDecl *assignedVarFromCallInStmt(const Stmt *S,
                                                StringRef CalleeName) {
  if (!S)
    return nullptr;
  if (const VarDecl *VD = assignedVarFromCall(S, CalleeName))
    return VD;
  for (const Stmt *Child : S->children())
    if (const VarDecl *VD = assignedVarFromCallInStmt(Child, CalleeName))
      return VD;
  return nullptr;
}

static bool callUsesSameSqObject(const CallExpr *ReadyCall, const Expr *CloseArg) {
  if (!ReadyCall || ReadyCall->getNumArgs() < 2 || !CloseArg)
    return false;
  return baseVar(ReadyCall->getArg(1)) &&
         baseVar(ReadyCall->getArg(1)) == baseVar(CloseArg);
}

bool SAGenTestChecker::isFailedReadyCleanupClose(const CallEvent &Call,
                                                 CheckerContext &C) const {
  const Expr *OriginExpr = Call.getOriginExpr();
  const CallExpr *CloseCE = asCallExpr(OriginExpr);
  if (!CloseCE || !calleeIs(CloseCE, "hws_send_ring_close_sq") ||
      CloseCE->getNumArgs() == 0)
    return false;

  const IfStmt *IfS = findSpecificTypeInParents<IfStmt>(OriginExpr, C);
  const CompoundStmt *Body = findSpecificTypeInParents<CompoundStmt>(OriginExpr, C);
  if (!IfS || !Body || !stmtContains(IfS->getThen(), OriginExpr))
    return false;

  llvm::SmallPtrSet<const VarDecl *, 4> CondVars;
  collectReferencedVars(IfS->getCond(), CondVars);
  if (CondVars.empty())
    return false;

  const VarDecl *ReadyErrVar = nullptr;
  const CallExpr *ReadyCall = nullptr;
  bool SawTargetIf = false;

  for (const Stmt *Child : llvm::reverse(Body->body())) {
    if (!SawTargetIf) {
      SawTargetIf = Child == IfS || stmtContains(Child, IfS);
      continue;
    }

    if (!ReadyErrVar) {
      const VarDecl *Assigned = assignedVarFromCallInStmt(Child,
                                                          "hws_send_ring_set_sq_rdy");
      const CallExpr *Candidate = findCallNamed(Child, "hws_send_ring_set_sq_rdy");
      if (Assigned && CondVars.count(Assigned) &&
          callUsesSameSqObject(Candidate, CloseCE->getArg(0))) {
        ReadyErrVar = Assigned;
        ReadyCall = Candidate;
      }
      continue;
    }

    const VarDecl *CreateErrVar = assignedVarFromCallInStmt(Child,
                                                            "hws_send_ring_create_sq");
    if (CreateErrVar == ReadyErrVar && findCallNamed(Child, "hws_send_ring_create_sq") &&
        ReadyCall)
      return true;
  }

  return false;
}

void SAGenTestChecker::checkPreCall(const CallEvent &Call,
                                    CheckerContext &C) const {
  if (!isFailedReadyCleanupClose(Call, C))
    return;

  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT,
      "Double-free risk: SQ software cleanup is used after hardware ready transition failure",
      N);
  Report->addRange(Call.getSourceRange());
  C.emitReport(std::move(Report));
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects SQ cleanup that frees software-owned buffers after ready transition failure",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
