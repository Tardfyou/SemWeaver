// /external/upstream-study/SAGEN/result-0224-bugfail-multi-o3mini/test-Uninit-Data-eaa03486d932572dfd1c5f64f9dfebe572ad88c0/checkers/checker4.cpp
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/AnalysisManager.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"

using namespace clang;
using namespace ento;

namespace {

static bool isTargetFunction(const FunctionDecl *FD) {
  if (!FD || !FD->getIdentifier())
    return false;
  StringRef Name = FD->getName();
  return Name == "regcache_maple_drop" || Name == "regcache_maple_sync";
}

static bool isNamedRetVar(const VarDecl *VD) {
  return VD && VD->getIdentifier() && VD->getName() == "ret";
}

static bool isPlainIntRet(const VarDecl *VD) {
  if (!isNamedRetVar(VD))
    return false;
  QualType Ty = VD->getType().getCanonicalType();
  return Ty->isIntegerType() && !Ty->isUnsignedIntegerType();
}

static bool isReturnOfVar(const ReturnStmt *RS, const VarDecl *Target) {
  if (!RS || !Target)
    return false;
  const Expr *Ret = RS->getRetValue();
  if (!Ret)
    return false;
  Ret = Ret->IgnoreParenImpCasts();
  const auto *DRE = dyn_cast<DeclRefExpr>(Ret);
  return DRE && DRE->getDecl() == Target;
}

class RetInitVisitor : public RecursiveASTVisitor<RetInitVisitor> {
  const VarDecl *UninitializedRet = nullptr;
  const ReturnStmt *ReturnSite = nullptr;

public:
  bool VisitVarDecl(const VarDecl *VD) {
    if (UninitializedRet || !isPlainIntRet(VD))
      return true;
    if (!VD->hasInit())
      UninitializedRet = VD;
    return true;
  }

  bool VisitReturnStmt(const ReturnStmt *RS) {
    if (!UninitializedRet || ReturnSite)
      return true;
    if (isReturnOfVar(RS, UninitializedRet))
      ReturnSite = RS;
    return true;
  }

  const VarDecl *getUninitializedRet() const { return UninitializedRet; }
  const ReturnStmt *getReturnSite() const { return ReturnSite; }
};

class SAGenTestChecker : public Checker<check::ASTDecl<FunctionDecl>> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Uninitialized regcache maple return value")) {}

  void checkASTDecl(const FunctionDecl *FD, AnalysisManager &Mgr,
                    BugReporter &BR) const {
    if (!isTargetFunction(FD) || !FD->hasBody())
      return;

    RetInitVisitor Visitor;
    Visitor.TraverseStmt(const_cast<Stmt *>(FD->getBody()));

    const VarDecl *BadRet = Visitor.getUninitializedRet();
    const ReturnStmt *ReturnSite = Visitor.getReturnSite();
    if (!BadRet || !ReturnSite)
      return;

    PathDiagnosticLocation Loc = PathDiagnosticLocation::createBegin(
        ReturnSite, BR.getSourceManager(), Mgr.getAnalysisDeclContext(FD));
    auto Report = std::make_unique<BasicBugReport>(
        *BT,
        "regcache maple function returns local ret without default initialization",
        Loc);
    Report->addRange(BadRet->getSourceRange());
    Report->addRange(ReturnSite->getSourceRange());
    BR.emitReport(std::move(Report));
  }
};

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects missing default initialization of regcache maple ret values",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
