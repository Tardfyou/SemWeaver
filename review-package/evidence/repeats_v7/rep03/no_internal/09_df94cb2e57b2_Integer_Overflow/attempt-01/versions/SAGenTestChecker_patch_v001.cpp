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

class WideIntegerMemberVisitor
    : public RecursiveASTVisitor<WideIntegerMemberVisitor> {
  const ASTContext &Context;
  bool FoundWideMember = false;

public:
  explicit WideIntegerMemberVisitor(const ASTContext &Context)
      : Context(Context) {}

  bool VisitMemberExpr(MemberExpr *ME) {
    QualType Type = ME->getType();
    if (Type->isIntegerType() && Context.getTypeSize(Type) > 32)
      FoundWideMember = true;
    return !FoundWideMember;
  }

  bool foundWideMember() const { return FoundWideMember; }
};

static bool isNarrowedWideMemberValue(const Expr *Expr,
                                      const ASTContext &Context) {
  Expr = Expr->IgnoreParenImpCasts();
  const auto *DRE = dyn_cast<DeclRefExpr>(Expr);
  if (!DRE)
    return false;

  const auto *VD = dyn_cast<VarDecl>(DRE->getDecl());
  if (!VD || !VD->hasInit() || !VD->getType()->isUnsignedIntegerType() ||
      Context.getTypeSize(VD->getType()) > 32)
    return false;

  WideIntegerMemberVisitor Visitor(Context);
  Visitor.TraverseStmt(VD->getInit());
  return Visitor.foundWideMember();
}

static bool formatUsesPlainUnsignedForArgument(StringRef Format,
                                                unsigned ArgumentIndex) {
  unsigned FormatArgument = 0;
  for (size_t I = 0; I < Format.size(); ++I) {
    if (Format[I] != '%')
      continue;
    if (++I == Format.size())
      break;
    if (Format[I] == '%')
      continue;

    while (I < Format.size() &&
           (Format[I] == '-' || Format[I] == '+' || Format[I] == ' ' ||
            Format[I] == '#' || Format[I] == '0'))
      ++I;
    if (I < Format.size() && Format[I] == '*') {
      ++FormatArgument;
      ++I;
    } else {
      while (I < Format.size() && Format[I] >= '0' && Format[I] <= '9')
        ++I;
    }
    if (I < Format.size() && Format[I] == '.') {
      ++I;
      if (I < Format.size() && Format[I] == '*') {
        ++FormatArgument;
        ++I;
      } else {
        while (I < Format.size() && Format[I] >= '0' && Format[I] <= '9')
          ++I;
      }
    }

    bool HasLengthModifier = false;
    if (I < Format.size() &&
        (Format[I] == 'h' || Format[I] == 'l' || Format[I] == 'j' ||
         Format[I] == 'z' || Format[I] == 't' || Format[I] == 'L')) {
      HasLengthModifier = true;
      char Modifier = Format[I++];
      if (I < Format.size() &&
          ((Modifier == 'h' && Format[I] == 'h') ||
           (Modifier == 'l' && Format[I] == 'l')))
        ++I;
    }
    if (I == Format.size())
      break;

    char Conversion = Format[I];
    if (Conversion == 'u' && !HasLengthModifier &&
        FormatArgument == ArgumentIndex)
      return true;
    if (Conversion != '%')
      ++FormatArgument;
  }
  return false;
}

/// Our checker class. We implement two callbacks:
/// 1. checkASTCodeBody to examine function bodies (and indirectly, declarations)
///    for variables whose type is unsigned int but whose names imply disk sector counts.
/// 2. checkPostCall to intercept calls to logging functions (like bch2_trans_inconsistent)
///    and analyze the format string for mismatched specifiers.
class SAGenTestChecker : public Checker<check::PostCall> {
  mutable std::unique_ptr<BugType> FormatBugTy;
public:
  SAGenTestChecker() {
    FormatBugTy.reset(new BugType(this, "Narrowed disk usage value in log",
                                  "Integer Overflow"));
  }

  // This callback inspects calls for logging functions with potential format
  // mismatches.
  void checkPostCall(const CallEvent &Call, CheckerContext &C) const {
    // Use the utility function ExprHasName for accurate function identification.
    const Expr *OriginExpr = Call.getOriginExpr();
    if (!OriginExpr)
      return;
    if (!ExprHasName(OriginExpr, "bch2_trans_inconsistent", C))
      return;

    for (unsigned i = 0; i < Call.getNumArgs(); ++i) {
      const Expr *FormatExpr = Call.getArgExpr(i);
      if (!FormatExpr)
        continue;
      FormatExpr = FormatExpr->IgnoreParenImpCasts();
      const auto *SL = dyn_cast<StringLiteral>(FormatExpr);
      if (!SL)
        continue;

      for (unsigned j = i + 1; j < Call.getNumArgs(); ++j) {
        const Expr *ValueExpr = Call.getArgExpr(j);
        if (!ValueExpr ||
            !formatUsesPlainUnsignedForArgument(SL->getString(), j - i - 1) ||
            !isNarrowedWideMemberValue(ValueExpr, C.getASTContext()))
          continue;

        ExplodedNode *N = C.generateNonFatalErrorNode();
        if (!N)
          return;
        auto Report = std::make_unique<PathSensitiveBugReport>(
            *FormatBugTy,
            "Integer overflow risk: wide disk usage value is narrowed before logging",
            N);
        Report->addRange(ValueExpr->getSourceRange());
        C.emitReport(std::move(Report));
        return;
      }
    }
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
