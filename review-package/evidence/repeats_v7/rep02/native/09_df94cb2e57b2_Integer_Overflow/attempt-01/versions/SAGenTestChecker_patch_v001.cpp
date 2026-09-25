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

/// Visits local storage declarations for the lossy conversion fixed by this
/// patch: a wide unsigned member value is stored in a 32-bit unsigned object.
class SectorVarVisitor : public RecursiveASTVisitor<SectorVarVisitor> {
  BugReporter &BR;
  const BugType &BugTy;

  bool isUncheckedWideMemberNarrowing(const VarDecl *VD) const {
    if (!VD->hasInit())
      return false;

    const auto *Choice = dyn_cast<ConditionalOperator>(
        VD->getInit()->IgnoreParenImpCasts());
    if (!Choice)
      return false;

    const Expr *Source = Choice->getTrueExpr()->IgnoreParenImpCasts();
    if (!isa<MemberExpr>(Source))
      return false;

    ASTContext &Ctx = VD->getASTContext();
    QualType DestinationType = VD->getType().getCanonicalType();
    QualType SourceType = Source->getType().getCanonicalType();
    return DestinationType->isUnsignedIntegerType() &&
           SourceType->isUnsignedIntegerType() &&
           Ctx.getTypeSize(DestinationType) == 32 &&
           Ctx.getTypeSize(SourceType) > Ctx.getTypeSize(DestinationType);
  }

public:
  SectorVarVisitor(BugReporter &BR, const BugType &BugTy) : BR(BR), BugTy(BugTy) {}

  bool VisitVarDecl(VarDecl *VD) {
    if (!isUncheckedWideMemberNarrowing(VD))
      return true;

    SmallString<128> Buf;
    llvm::raw_svector_ostream OS(Buf);
    OS << "A wide unsigned member value is narrowed to 32-bit storage without "
          "a range check";
    PathDiagnosticLocation Loc(VD, BR.getSourceManager());
    auto Report = std::make_unique<BasicBugReport>(BugTy, OS.str(), Loc);
    Report->addRange(VD->getSourceRange());
    BR.emitReport(std::move(Report));
    return true;
  }
};

class SAGenTestChecker : public Checker<check::ASTCodeBody> {
  mutable std::unique_ptr<BugType> DeclBugTy;

public:
  SAGenTestChecker()
      : DeclBugTy(new BugType(this, "Unchecked unsigned narrowing", "Integer Overflow")) {}

  void checkASTCodeBody(const Decl *D, AnalysisManager &Mgr, BugReporter &BR) const {
    const auto *FD = dyn_cast<FunctionDecl>(D);
    if (!FD || !FD->hasBody())
      return;

    SectorVarVisitor Visitor(BR, *DeclBugTy);
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
