// /external/upstream-study/SAGEN/result-0224-bugfail-multi-o3mini/test-Integer-Overflow-df94cb2e57b2cc539f325003e7abb76d3060d55b/checkers/checker7.cpp
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"
// Removed include "clang/StaticAnalyzer/Checkers/Checkers.h" because it does not exist in Clang-18
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/StaticAnalyzer/Checkers/utility.h"
#include <memory>

using namespace clang;
using namespace ento;

namespace {

/// Finds local initializations that discard the range of a wider integral value.
class NarrowingIntegralVisitor
    : public RecursiveASTVisitor<NarrowingIntegralVisitor> {
  ASTContext &ACtx;
  BugReporter &BR;
  BugType &BugTy;

public:
  NarrowingIntegralVisitor(ASTContext &ACtx, BugReporter &BR, BugType &BugTy)
      : ACtx(ACtx), BR(BR), BugTy(BugTy) {}

  bool VisitVarDecl(VarDecl *VD) {
    const Expr *Init = VD->getInit();
    if (!Init || !VD->getType()->isIntegerType())
      return true;

    const Expr *Source = Init->IgnoreParenImpCasts();
    if (!Source->getType()->isIntegerType())
      return true;

    if (ACtx.getIntWidth(VD->getType()) >=
        ACtx.getIntWidth(Source->getType()))
      return true;

    PathDiagnosticLocation DLoc =
        PathDiagnosticLocation::createBegin(VD, BR.getSourceManager());
    auto Report = std::make_unique<BasicBugReport>(
        BugTy, "Wider integral value is narrowed during initialization", DLoc);
    Report->addRange(VD->getSourceRange());
    BR.emitReport(std::move(Report));
    return true;
  }
};

class SAGenTestChecker : public Checker<check::ASTCodeBody> {
  mutable std::unique_ptr<BugType> NarrowingBugTy;

public:
  SAGenTestChecker()
      : NarrowingBugTy(new BugType(this, "Narrowing integral initialization",
                                   "Integer Overflow")) {}

  void checkASTCodeBody(const Decl *D, AnalysisManager &Mgr,
                        BugReporter &BR) const {
    const FunctionDecl *FD = dyn_cast<FunctionDecl>(D);
    if (!FD || !FD->hasBody())
      return;

    NarrowingIntegralVisitor Visitor(Mgr.getASTContext(), BR, *NarrowingBugTy);
    Visitor.TraverseStmt(FD->getBody());
  }
};

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker", 
      "Checks for use of insufficient integer width for disk sector counts "
      "and mismatched format specifiers in logging", "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
