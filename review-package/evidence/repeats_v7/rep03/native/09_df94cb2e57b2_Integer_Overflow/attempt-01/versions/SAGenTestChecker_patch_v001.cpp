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

/// Finds a lossy unsigned conversion that is subsequently consumed by the
/// corresponding unqualified %u conversion of a variadic diagnostic call.
class IntegerNarrowingVisitor
    : public RecursiveASTVisitor<IntegerNarrowingVisitor> {
  BugReporter &BR;
  const BugType &BugTy;

  static bool isNarrowingUnsigned(const VarDecl *VD) {
    if (!VD->hasInit())
      return false;

    QualType Destination = VD->getType().getCanonicalType();
    const Expr *Source = VD->getInit()->IgnoreParenImpCasts();
    QualType SourceType = Source->getType().getCanonicalType();
    if (!Destination->isUnsignedIntegerType() ||
        !SourceType->isUnsignedIntegerType())
      return false;

    ASTContext &Ctx = VD->getASTContext();
    return Ctx.getTypeSize(SourceType) > Ctx.getTypeSize(Destination);
  }

  static unsigned unqualifiedUnsignedArgument(llvm::StringRef Format) {
    unsigned Argument = 0;
    for (size_t I = 0; I < Format.size();) {
      if (Format[I++] != '%')
        continue;
      if (I == Format.size() || Format[I] == '%') {
        if (I < Format.size())
          ++I;
        continue;
      }

      while (I < Format.size() &&
             (Format[I] == '-' || Format[I] == '+' || Format[I] == ' ' ||
              Format[I] == '#' || Format[I] == '0'))
        ++I;
      while (I < Format.size() && Format[I] >= '0' && Format[I] <= '9')
        ++I;
      if (I < Format.size() && Format[I] == '.') {
        ++I;
        while (I < Format.size() && Format[I] >= '0' && Format[I] <= '9')
          ++I;
      }

      bool HasLengthModifier = false;
      while (I < Format.size() &&
             (Format[I] == 'h' || Format[I] == 'l' || Format[I] == 'j' ||
              Format[I] == 'z' || Format[I] == 't' || Format[I] == 'L')) {
        HasLengthModifier = true;
        ++I;
      }
      if (I == Format.size())
        break;

      char Conversion = Format[I++];
      ++Argument;
      if (Conversion == 'u' && !HasLengthModifier)
        return Argument;
    }
    return 0;
  }

public:
  IntegerNarrowingVisitor(BugReporter &BR, const BugType &BugTy)
      : BR(BR), BugTy(BugTy) {}

  bool VisitCallExpr(CallExpr *Call) {
    const FunctionDecl *Callee = Call->getDirectCallee();
    if (!Callee || !Callee->isVariadic())
      return true;

    for (unsigned FormatIndex = 0; FormatIndex < Call->getNumArgs();
         ++FormatIndex) {
      const Expr *FormatExpr =
          Call->getArg(FormatIndex)->IgnoreParenImpCasts();
      const auto *Format = dyn_cast<StringLiteral>(FormatExpr);
      if (!Format)
        continue;

      unsigned FormatArgument = unqualifiedUnsignedArgument(Format->getString());
      if (!FormatArgument || FormatIndex + FormatArgument >= Call->getNumArgs())
        continue;

      const Expr *ValueExpr =
          Call->getArg(FormatIndex + FormatArgument)->IgnoreParenImpCasts();
      const auto *Value = dyn_cast<DeclRefExpr>(ValueExpr);
      const auto *VD = Value ? dyn_cast<VarDecl>(Value->getDecl()) : nullptr;
      if (!VD || !isNarrowingUnsigned(VD))
        continue;

      PathDiagnosticLocation DLoc =
          PathDiagnosticLocation::createBegin(VD, BR.getSourceManager());
      auto Report = std::make_unique<BasicBugReport>(
          BugTy,
          "A wider unsigned value is truncated before being passed to a %u "
          "diagnostic conversion",
          DLoc);
      Report->addRange(VD->getSourceRange());
      Report->addRange(ValueExpr->getSourceRange());
      BR.emitReport(std::move(Report));
      return true;
    }
    return true;
  }
};

class SAGenTestChecker : public Checker<check::ASTCodeBody> {
  mutable std::unique_ptr<BugType> NarrowingBugTy;

public:
  SAGenTestChecker()
      : NarrowingBugTy(std::make_unique<BugType>(
            this, "Lossy unsigned diagnostic value", "Integer Overflow")) {}

  void checkASTCodeBody(const Decl *D, AnalysisManager &Mgr,
                        BugReporter &BR) const {
    const auto *FD = dyn_cast<FunctionDecl>(D);
    if (!FD || !FD->hasBody())
      return;

    IntegerNarrowingVisitor Visitor(BR, *NarrowingBugTy);
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
