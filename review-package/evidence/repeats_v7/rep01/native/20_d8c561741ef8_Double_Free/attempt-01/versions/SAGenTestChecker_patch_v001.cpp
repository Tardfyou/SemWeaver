// /external/upstream-study/SAGEN/result-0224-bugfail-multi-o3mini/test-Double-Free-d8c561741ef83980114b3f7f95ffac54600f3f16/checkers/checker2.cpp
#include "clang/AST/Expr.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include "llvm/ADT/StringRef.h"

using namespace clang;
using namespace ento;

namespace {

static const VarDecl *referencedVar(const Expr *E) {
  E = E->IgnoreParenImpCasts();
  if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
    return dyn_cast<VarDecl>(DRE->getDecl());
  return nullptr;
}

static const VarDecl *baseVar(const Expr *E) {
  E = E->IgnoreParenImpCasts();
  while (const auto *ME = dyn_cast<MemberExpr>(E))
    E = ME->getBase()->IgnoreParenImpCasts();
  return referencedVar(E);
}

static bool isNamedCall(const Expr *E, llvm::StringRef Name) {
  E = E->IgnoreParenImpCasts();
  const auto *CE = dyn_cast<CallExpr>(E);
  const FunctionDecl *FD = CE ? CE->getDirectCallee() : nullptr;
  return FD && FD->getName() == Name;
}

static bool containsStmt(const Stmt *Root, const Stmt *Needle) {
  if (!Root)
    return false;
  if (Root == Needle)
    return true;
  for (const Stmt *Child : Root->children())
    if (containsStmt(Child, Needle))
      return true;
  return false;
}

static const IfStmt *enclosingIf(const Stmt *S, ASTContext &AC) {
  const Stmt *Current = S;
  while (Current) {
    auto Parents = AC.getParents(*Current);
    if (Parents.empty())
      return nullptr;
    if (const auto *IS = Parents[0].get<IfStmt>())
      return IS;
    Current = Parents[0].get<Stmt>();
  }
  return nullptr;
}

class SQCreationVisitor : public RecursiveASTVisitor<SQCreationVisitor> {
  const VarDecl *Error;
  const VarDecl *SQ;
  SourceLocation CleanupLoc;
  const SourceManager &SM;
  bool SawReadyFailure = false;
  bool SawCreatedSQ = false;

  bool precedesCleanup(SourceLocation Loc) const {
    return Loc.isValid() && CleanupLoc.isValid() &&
           SM.isBeforeInTranslationUnit(Loc, CleanupLoc);
  }

public:
  SQCreationVisitor(const VarDecl *Error, const VarDecl *SQ,
                    SourceLocation CleanupLoc, const SourceManager &SM)
      : Error(Error), SQ(SQ), CleanupLoc(CleanupLoc), SM(SM) {}

  bool VisitBinaryOperator(BinaryOperator *BO) {
    if (BO->getOpcode() != BO_Assign ||
        referencedVar(BO->getLHS()) != Error ||
        !precedesCleanup(BO->getBeginLoc()))
      return true;

    const Expr *RHS = BO->getRHS()->IgnoreParenImpCasts();
    const auto *CE = dyn_cast<CallExpr>(RHS);
    if (CE && isNamedCall(CE, "hws_send_ring_set_sq_rdy") &&
        CE->getNumArgs() == 2 && baseVar(CE->getArg(1)) == SQ)
      SawReadyFailure = true;
    return true;
  }

  bool VisitCallExpr(CallExpr *CE) {
    if (!precedesCleanup(CE->getBeginLoc()) ||
        !isNamedCall(CE, "hws_send_ring_create_sq") ||
        CE->getNumArgs() < 5)
      return true;

    if (baseVar(CE->getArg(4)) == SQ)
      SawCreatedSQ = true;
    return true;
  }

  bool sawRequiredLifecycle() const {
    return SawReadyFailure && SawCreatedSQ;
  }
};

class SAGenTestChecker : public Checker<check::PreCall> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Double-free after SQ readiness failure")) {}

  void checkPreCall(const CallEvent &Call, CheckerContext &C) const;
};

void SAGenTestChecker::checkPreCall(const CallEvent &Call,
                                    CheckerContext &C) const {
  const Expr *OriginExpr = Call.getOriginExpr();
  if (!OriginExpr || !isNamedCall(OriginExpr, "hws_send_ring_close_sq"))
    return;

  const auto *Cleanup = dyn_cast<CallExpr>(OriginExpr->IgnoreParenImpCasts());
  if (!Cleanup || Cleanup->getNumArgs() != 1)
    return;
  const VarDecl *SQ = baseVar(Cleanup->getArg(0));
  if (!SQ)
    return;

  const IfStmt *Guard = enclosingIf(Cleanup, C.getASTContext());
  if (!Guard)
    return;

  const Expr *Condition = Guard->getCond()->IgnoreParenImpCasts();
  const VarDecl *Error = referencedVar(Condition);
  bool IsFailureBranch = Error && containsStmt(Guard->getThen(), Cleanup);
  if (const auto *UO = dyn_cast<UnaryOperator>(Condition)) {
    if (UO->getOpcode() == UO_LNot) {
      Error = referencedVar(UO->getSubExpr());
      IsFailureBranch = Error && containsStmt(Guard->getElse(), Cleanup);
    }
  }
  if (!IsFailureBranch)
    return;

  const auto *FD = dyn_cast<FunctionDecl>(C.getLocationContext()->getDecl());
  if (!FD)
    return;

  SQCreationVisitor Visitor(Error, SQ, Cleanup->getBeginLoc(),
                            C.getSourceManager());
  Visitor.TraverseDecl(const_cast<FunctionDecl *>(FD));
  if (!Visitor.sawRequiredLifecycle())
    return;

  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT,
      "Closing an SQ after readiness failure frees resources owned by the SQ creation path",
      N);
  Report->addRange(Call.getSourceRange());
  C.emitReport(std::move(Report));
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects invalid cleanup of a created SQ after readiness failure", "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
