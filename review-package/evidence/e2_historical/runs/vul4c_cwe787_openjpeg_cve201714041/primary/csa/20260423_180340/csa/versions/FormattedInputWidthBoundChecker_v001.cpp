#include "clang/AST/Expr.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/Version.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/CheckerManager.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/MemRegion.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SVals.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include "llvm/ADT/StringRef.h"
#include <cctype>
#include <memory>

using namespace clang;
using namespace ento;

namespace {

struct FormatSpecInfo {
  bool Valid = false;
  bool Suppressed = false;
  bool HasWidth = false;
  bool IsScanset = false;
  unsigned Width = 0;
  unsigned ConsumedChars = 0;
};

static const Expr *stripExpr(const Expr *E) {
  if (!E)
    return nullptr;
  return E->IgnoreParenImpCasts();
}

static bool isScanfFamily(StringRef Name) {
  return Name == "scanf" || Name == "fscanf" || Name == "sscanf" ||
         Name == "vscanf" || Name == "vfscanf" || Name == "vsscanf";
}

static int getFormatArgIndex(StringRef Name) {
  if (Name == "scanf" || Name == "vscanf")
    return 0;
  if (Name == "fscanf" || Name == "vfscanf" || Name == "sscanf" ||
      Name == "vsscanf")
    return 1;
  return -1;
}

static bool isDigitChar(char C) {
  return std::isdigit(static_cast<unsigned char>(C)) != 0;
}

static FormatSpecInfo parseFormatSpec(StringRef Format, size_t PercentPos) {
  FormatSpecInfo Info;
  if (PercentPos >= Format.size() || Format[PercentPos] != '%')
    return Info;

  size_t I = PercentPos + 1;
  if (I >= Format.size())
    return Info;

  if (Format[I] == '%') {
    Info.Valid = true;
    Info.ConsumedChars = 2;
    return Info;
  }

  if (Format[I] == '*') {
    Info.Suppressed = true;
    ++I;
  }

  if (I < Format.size() && isDigitChar(Format[I])) {
    Info.HasWidth = true;
    unsigned Width = 0;
    while (I < Format.size() && isDigitChar(Format[I])) {
      Width = Width * 10 + static_cast<unsigned>(Format[I] - '0');
      ++I;
    }
    Info.Width = Width;
  }

  while (I < Format.size()) {
    char C = Format[I];
    if (C == 'h' || C == 'l' || C == 'L' || C == 'j' || C == 'z' || C == 't' ||
        C == 'q') {
      ++I;
      if (I < Format.size()) {
        if ((C == 'h' && Format[I] == 'h') || (C == 'l' && Format[I] == 'l'))
          ++I;
      }
      continue;
    }
    break;
  }

  if (I >= Format.size())
    return Info;

  if (Format[I] == '[') {
    Info.IsScanset = true;
    ++I;
    if (I < Format.size() && (Format[I] == '^' || Format[I] == ']'))
      ++I;
    while (I < Format.size() && Format[I] != ']')
      ++I;
    if (I >= Format.size())
      return Info;
    ++I;
    Info.Valid = true;
    Info.ConsumedChars = static_cast<unsigned>(I - PercentPos);
    return Info;
  }

  Info.Valid = true;
  Info.ConsumedChars = static_cast<unsigned>(I - PercentPos + 1);
  return Info;
}

static const StringLiteral *getConstantFormatString(const CallEvent &Call,
                                                    int FormatArgIndex) {
  if (FormatArgIndex < 0)
    return nullptr;
  if (static_cast<unsigned>(FormatArgIndex) >= Call.getNumArgs())
    return nullptr;
  const Expr *Arg = Call.getArgExpr(static_cast<unsigned>(FormatArgIndex));
  Arg = stripExpr(Arg);
  if (!Arg)
    return nullptr;
  return dyn_cast<StringLiteral>(Arg);
}

static const MemRegion *getBaseRegionFromArg(const Expr *Arg,
                                             CheckerContext &C) {
  Arg = stripExpr(Arg);
  if (!Arg)
    return nullptr;
  SVal V = C.getSVal(Arg);
  std::optional<loc::MemRegionVal> MRV = V.getAs<loc::MemRegionVal>();
  if (!MRV)
    return nullptr;
  const MemRegion *MR = MRV->getRegion();
  if (!MR)
    return nullptr;
  return MR->StripCasts();
}

static bool isLikelyCharBufferArg(const Expr *Arg, CheckerContext &C) {
  const Expr *E = stripExpr(Arg);
  if (!E)
    return false;

  QualType QT = E->getType();
  if (!QT.isNull()) {
    if (const auto *AT = C.getASTContext().getAsArrayType(QT)) {
      QualType Elem = AT->getElementType();
      if (!Elem.isNull() && Elem->isAnyCharacterType())
        return true;
    }
    if (QT->isPointerType()) {
      QualType Pointee = QT->getPointeeType();
      if (!Pointee.isNull() && Pointee->isAnyCharacterType())
        return true;
    }
  }

  const MemRegion *MR = getBaseRegionFromArg(E, C);
  if (!MR)
    return false;
  if (const auto *TVR = dyn_cast<TypedValueRegion>(MR)) {
    QualType RQT = TVR->getValueType();
    if (!RQT.isNull()) {
      if (const auto *AT = C.getASTContext().getAsArrayType(RQT)) {
        QualType Elem = AT->getElementType();
        if (!Elem.isNull() && Elem->isAnyCharacterType())
          return true;
      }
      if (RQT->isPointerType()) {
        QualType Pointee = RQT->getPointeeType();
        if (!Pointee.isNull() && Pointee->isAnyCharacterType())
          return true;
      }
    }
  }
  return false;
}

class FormattedInputWidthBoundChecker : public Checker<check::PreCall> {
  mutable std::unique_ptr<BugType> BT;

public:
  FormattedInputWidthBoundChecker()
      : BT(std::make_unique<BugType>(this,
                                     "Unbounded scanset in formatted input",
                                     "Memory safety")) {}

  void checkPreCall(const CallEvent &Call, CheckerContext &C) const {
    const IdentifierInfo *II = Call.getCalleeIdentifier();
    if (!II)
      return;

    StringRef FuncName = II->getName();
    if (!isScanfFamily(FuncName))
      return;

    int FormatArgIndex = getFormatArgIndex(FuncName);
    const StringLiteral *Fmt = getConstantFormatString(Call, FormatArgIndex);
    if (!Fmt)
      return;

    StringRef Format = Fmt->getString();
    unsigned DataArgIndex = static_cast<unsigned>(FormatArgIndex + 1);

    for (size_t I = 0; I < Format.size(); ++I) {
      if (Format[I] != '%')
        continue;

      FormatSpecInfo Spec = parseFormatSpec(Format, I);
      if (!Spec.Valid)
        continue;

      if (Spec.ConsumedChars > 0)
        I += Spec.ConsumedChars - 1;

      if (!Spec.IsScanset || Spec.Suppressed || Spec.HasWidth)
        continue;

      if (DataArgIndex >= Call.getNumArgs())
        return;

      const Expr *TargetArg = Call.getArgExpr(DataArgIndex);
      if (!TargetArg)
        return;

      if (!isLikelyCharBufferArg(TargetArg, C)) {
        ++DataArgIndex;
        continue;
      }

      ExplodedNode *N = C.generateNonFatalErrorNode();
      if (!N)
        return;

      auto R = std::make_unique<PathSensitiveBugReport>(
          *BT,
          "Formatted input uses an unbounded %[ scanset that may overflow the destination character buffer.",
          N);
      R->addRange(TargetArg->getSourceRange());
      C.emitReport(std::move(R));
      return;
    }
  }
};

} // namespace

extern "C" void clang_registerCheckers(CheckerRegistry &Registry) {
  Registry.addChecker<FormattedInputWidthBoundChecker>(
      "custom.FormattedInputWidthBoundChecker",
      "Detect unbounded %[ scansets in scanf-family calls.",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] = CLANG_VERSION_STRING;
