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
#include <set>

using namespace clang;
using namespace ento;

namespace {

/// Finds a full-width integer that is implicitly truncated before it is
/// compared with another full-width value.
class LossyIntegerConversionVisitor
    : public RecursiveASTVisitor<LossyIntegerConversionVisitor> {
  BugReporter &BR;
  BugType &BugTy;
  ASTContext &Ctx;
  std::set<const VarDecl *> NarrowedDecls;
  std::set<const VarDecl *> ReportedDecls;

  bool isImplicitNarrowing(const VarDecl *VD) const {
    const Expr *Init = VD->getInit();
    if (!Init || !VD->getType()->isUnsignedIntegerType())
      return false;

    const auto *Cast = dyn_cast<ImplicitCastExpr>(Init);
    if (!Cast || Cast->getCastKind() != CK_IntegralCast)
      return false;

    QualType SourceType = Cast->getSubExpr()->getType().getCanonicalType();
    QualType DestinationType = VD->getType().getCanonicalType();
    return SourceType->isIntegerType() &&
           Ctx.getTypeSize(SourceType) > Ctx.getTypeSize(DestinationType);
  }

  void report(const VarDecl *VD, const BinaryOperator *Comparison) {
    if (!ReportedDecls.insert(VD).second)
      return;

    PathDiagnosticLocation Loc = PathDiagnosticLocation::createSingleLocation(
        VD->getLocation(), BR.getSourceManager());
    auto Report = std::make_unique<BasicBugReport>(
        BugTy,
        "A 64-bit integer is implicitly narrowed to 32 bits before a comparison with a wider value",
        Loc);
    Report->addRange(Comparison->getSourceRange());
    BR.emitReport(std::move(Report));
  }

public:
  LossyIntegerConversionVisitor(BugReporter &BR, BugType &BugTy,
                                ASTContext &Ctx)
      : BR(BR), BugTy(BugTy), Ctx(Ctx) {}

  bool VisitVarDecl(VarDecl *VD) {
    if (isImplicitNarrowing(VD))
      NarrowedDecls.insert(VD);
    return true;
  }

  bool VisitBinaryOperator(BinaryOperator *BO) {
    if (!BO->isComparisonOp())
      return true;

    const Expr *LHS = BO->getLHS()->IgnoreParenImpCasts();
    const Expr *RHS = BO->getRHS()->IgnoreParenImpCasts();
    const auto *LHSRef = dyn_cast<DeclRefExpr>(LHS);
    const auto *RHSRef = dyn_cast<DeclRefExpr>(RHS);

    const VarDecl *Narrowed = nullptr;
    const Expr *Other = nullptr;
    if (LHSRef) {
      if (const auto *VD = dyn_cast<VarDecl>(LHSRef->getDecl())) {
        if (NarrowedDecls.count(VD)) {
          Narrowed = VD;
          Other = RHS;
        }
      }
    }
    if (!Narrowed && RHSRef) {
      if (const auto *VD = dyn_cast<VarDecl>(RHSRef->getDecl())) {
        if (NarrowedDecls.count(VD)) {
          Narrowed = VD;
          Other = LHS;
        }
      }
    }

    if (Narrowed && Other->getType()->isIntegerType() &&
        Ctx.getTypeSize(Other->getType()) > Ctx.getTypeSize(Narrowed->getType()))
      report(Narrowed, BO);
    return true;
  }
};

class SAGenTestChecker : public Checker<check::ASTCodeBody> {
  mutable std::unique_ptr<BugType> ConversionBugTy;

public:
  SAGenTestChecker()
      : ConversionBugTy(new BugType(this, "Lossy integer comparison",
                                    "Integer Overflow")) {}

  void checkASTCodeBody(const Decl *D, AnalysisManager &Mgr,
                        BugReporter &BR) const {
    if (const auto *FD = dyn_cast<FunctionDecl>(D)) {
      if (FD->hasBody()) {
        LossyIntegerConversionVisitor Visitor(BR, *ConversionBugTy,
                                              FD->getASTContext());
        Visitor.TraverseStmt(FD->getBody());
      }
    }
  }
};

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker", 
      "Checks for implicit narrowing before comparison with a wider integer", "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
