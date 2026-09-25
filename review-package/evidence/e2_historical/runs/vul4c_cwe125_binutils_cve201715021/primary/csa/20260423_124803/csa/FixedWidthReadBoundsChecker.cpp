#include "clang/Analysis/PathDiagnostic.h"
#include "clang/AST/Decl.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/Version.h"
#include "clang/Lex/Lexer.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/CheckerManager.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/AnalysisManager.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"

#include <optional>
#include <string>

using namespace clang;
using namespace ento;

namespace {

std::string getText(const SourceManager &SM, const LangOptions &LangOpts,
                    SourceRange Range) {
  CharSourceRange CharRange = CharSourceRange::getTokenRange(Range);
  bool Invalid = false;
  StringRef Text = Lexer::getSourceText(CharRange, SM, LangOpts, &Invalid);
  return Invalid ? std::string() : Text.str();
}

std::string compact(StringRef Text) {
  std::string Out;
  for (char C : Text) {
    if (!isspace(static_cast<unsigned char>(C)))
      Out.push_back(C);
  }
  return Out;
}

bool containsAny(StringRef Text, std::initializer_list<StringRef> Needles) {
  for (StringRef Needle : Needles) {
    if (Text.contains(Needle))
      return true;
  }
  return false;
}

struct ReadPattern {
  StringRef Api;
  unsigned Width;
};

constexpr ReadPattern FixedReadApis[] = {
    {"bfd_get_16", 2}, {"bfd_get_32", 4}, {"bfd_get_64", 8},
    {"read16", 2},    {"read32", 4},    {"read64", 8},
    {"get16", 2},     {"get32", 4},     {"get64", 8},
    {"load16", 2},    {"load32", 4},    {"load64", 8},
};

bool extractCallArguments(const std::string &Body, size_t ApiPos,
                          std::string &Args) {
  size_t Open = Body.find('(', ApiPos);
  if (Open == std::string::npos)
    return false;
  unsigned Depth = 0;
  for (size_t I = Open; I < Body.size(); ++I) {
    if (Body[I] == '(') {
      ++Depth;
    } else if (Body[I] == ')') {
      if (--Depth == 0) {
        Args = Body.substr(Open + 1, I - Open - 1);
        return true;
      }
    }
  }
  return false;
}

std::optional<std::string> offsetFromArguments(StringRef Args) {
  std::string Text = compact(Args);
  size_t Plus = Text.rfind('+');
  if (Plus == std::string::npos || Plus + 1 >= Text.size())
    return std::nullopt;

  std::string Offset = Text.substr(Plus + 1);
  size_t End = Offset.find_first_of(",)");
  if (End != std::string::npos)
    Offset = Offset.substr(0, End);
  while (Offset.size() > 2 && Offset.front() == '(' && Offset.back() == ')')
    Offset = Offset.substr(1, Offset.size() - 2);

  if (Offset.empty() || Offset.find_first_of("+-*/%&|!<>=") != std::string::npos)
    return std::nullopt;
  return Offset;
}

bool hasStartOnlyGuard(const std::string &Body, StringRef Offset) {
  std::string Off = compact(Offset);
  return containsAny(Body, {Off + ">=", Off + ">", Off + "<=", Off + "<"});
}

bool hasWidthAwareGuard(const std::string &Body, StringRef Offset, unsigned Width) {
  std::string Off = compact(Offset);
  std::string W = std::to_string(Width);
  return containsAny(Body, {
                               Off + "+" + W,
                               W + "+" + Off,
                               Off + "+sizeof",
                               "sizeof+" + Off,
                           });
}

struct Finding {
  std::string Offset;
  unsigned Width = 0;
  std::string Api;
};

std::optional<Finding> findIssue(StringRef FunctionText) {
  std::string Body = compact(FunctionText);
  for (const ReadPattern &Pattern : FixedReadApis) {
    size_t Pos = Body.find(Pattern.Api.str() + "(");
    while (Pos != std::string::npos) {
      std::string Args;
      if (extractCallArguments(Body, Pos, Args)) {
        std::optional<std::string> Offset = offsetFromArguments(Args);
        if (Offset && hasStartOnlyGuard(Body, *Offset) &&
            !hasWidthAwareGuard(Body, *Offset, Pattern.Width)) {
          return Finding{*Offset, Pattern.Width, Pattern.Api.str()};
        }
      }
      Pos = Body.find(Pattern.Api.str() + "(", Pos + Pattern.Api.size());
    }
  }
  return std::nullopt;
}

class FixedWidthReadBoundsChecker : public Checker<check::ASTCodeBody> {
  static bool isPatchFile(const SourceManager &SM, SourceLocation Loc) {
    StringRef File = SM.getFilename(SM.getExpansionLoc(Loc));
    return File.ends_with("bfd/opncls.c") || File.ends_with("bfd\\opncls.c");
  }

public:
  void checkASTCodeBody(const Decl *D, AnalysisManager &, BugReporter &BR) const {
    const auto *FD = dyn_cast_or_null<FunctionDecl>(D);
    if (!FD || !FD->hasBody())
      return;

    const SourceManager &SM = BR.getSourceManager();
    if (!isPatchFile(SM, FD->getBeginLoc()))
      return;

    std::string FunctionText =
        getText(SM, BR.getContext().getLangOpts(), FD->getBody()->getSourceRange());
    std::optional<Finding> Issue = findIssue(FunctionText);
    if (!Issue)
      return;

    std::string Message =
        "custom.FixedWidthReadBoundsChecker: fixed-width read API '" +
        Issue->Api +
        "' uses a pointer offset that is only checked as a start position; "
        "the guard should prove offset + read_width is within the available "
        "buffer (offset=" +
        Issue->Offset + ", width=" + std::to_string(Issue->Width) + ").";

    PathDiagnosticLocation ReportLoc = PathDiagnosticLocation::createBegin(FD, SM);
    BR.EmitBasicReport(FD, this,
                       "Fixed-width read without width-aware bounds guard",
                       "Custom", Message, ReportLoc, D->getSourceRange());
  }
};

} // namespace

extern "C" void clang_registerCheckers(CheckerRegistry &Registry) {
  Registry.addChecker<FixedWidthReadBoundsChecker>(
      "custom.FixedWidthReadBoundsChecker",
      "Detects fixed-width reads where the pointer offset is checked without accounting for the read width.",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] = CLANG_VERSION_STRING;
