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

/// Finds actual lossy assignments from a wider unsigned value into a
/// 32-bit unsigned local. This is the truncation fixed by widening
/// disk_res_sectors to u64.
class SectorVarVisitor : public RecursiveASTVisitor<SectorVarVisitor> {
  BugReporter &BR;
  BugType &BugTy;

public:
  SectorVarVisitor(BugReporter &BR, BugType &BugTy) : BR(BR), BugTy(BugTy) {}

  bool VisitVarDecl(VarDecl *VD) {
    const Expr *Init = VD->getInit();
    if (!Init)
      return true;

    QualType DestType = VD->getType().getCanonicalType();
    const BuiltinType *DestBuiltin =
        dyn_cast<BuiltinType>(DestType.getTypePtr());
    if (!DestBuiltin || DestBuiltin->getKind() != BuiltinType::UInt)
      return true;

    const Expr *Source = Init->IgnoreParenImpCasts();
    QualType SourceType = Source->getType().getCanonicalType();
    if (!SourceType->isUnsignedIntegerType() ||
        VD->getASTContext().getTypeSize(SourceType) <=
            VD->getASTContext().getTypeSize(DestType))
      return true;

    PathDiagnosticLocation DLoc(VD->getLocation(), BR.getSourceManager());
    auto Report = std::make_unique<BasicBugReport>(
        BugTy,
        "32-bit unsigned variable truncates a wider unsigned sector value",
        DLoc);
    Report->addRange(VD->getSourceRange());
    BR.emitReport(std::move(Report));
    return true;
  }
};

/// This checker reports the lossy numeric conversion itself, rather than a
/// variable spelling or the logging call that later exposes the wrong value.
class SAGenTestChecker : public Checker<check::ASTCodeBody> {
  mutable std::unique_ptr<BugType> DeclBugTy;

public:
  SAGenTestChecker() {
    DeclBugTy.reset(new BugType(this, "Lossy sector count narrowing",
                                "Integer Overflow"));
  }

  void checkASTCodeBody(const Decl *D, AnalysisManager &Mgr,
                        BugReporter &BR) const {
    if (const FunctionDecl *FD = dyn_cast<FunctionDecl>(D)) {
      if (FD->hasBody()) {
        SectorVarVisitor Visitor(BR, *DeclBugTy);
        Visitor.TraverseStmt(FD->getBody());
      }
    }
  }
};

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker", 
      "Checks for lossy narrowing of wider unsigned sector counts", "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
