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
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/StaticAnalyzer/Checkers/utility.h"
#include <cctype>
#include <memory>
#include <string>

using namespace clang;
using namespace ento;

namespace {

static std::string getLowerCaseName(const NamedDecl *D) {
  std::string Name = D->getNameAsString();

  for (char &Ch : Name)
    Ch = static_cast<char>(
        std::tolower(static_cast<unsigned char>(Ch)));

  return Name;
}

static bool hasSectorName(const NamedDecl *D) {
  if (!D || !D->getIdentifier())
    return false;

  return getLowerCaseName(D).find("sector") != std::string::npos;
}

static bool isUnsignedIntType(QualType QT) {
  QT = QT.getCanonicalType().getUnqualifiedType();

  const auto *BT = dyn_cast<BuiltinType>(QT.getTypePtr());
  return BT && BT->getKind() == BuiltinType::UInt;
}

static bool isWideIntegerType(QualType QT, const ASTContext &ACtx) {
  QT = QT.getCanonicalType().getUnqualifiedType();

  return QT->isIntegerType() &&
         ACtx.getTypeSize(QT) > ACtx.getTypeSize(ACtx.UnsignedIntTy);
}

static const Expr *stripAllCasts(const Expr *E) {
  if (!E)
    return nullptr;

  E = E->IgnoreParens();

  while (const auto *CE = dyn_cast<CastExpr>(E))
    E = CE->getSubExpr()->IgnoreParens();

  return E;
}

static bool isWideSourceExpression(const Expr *E, const ASTContext &ACtx) {
  E = stripAllCasts(E);
  return E && isWideIntegerType(E->getType(), ACtx);
}

static StringRef getSourceText(const SourceRange &Range,
                               const ASTContext &ACtx) {
  const SourceManager &SM = ACtx.getSourceManager();
  const LangOptions &LangOpts = ACtx.getLangOpts();

  return Lexer::getSourceText(
      CharSourceRange::getTokenRange(Range), SM, LangOpts);
}

static bool isMacroNamed(SourceLocation Loc, StringRef Name,
                         const ASTContext &ACtx) {
  if (!Loc.isMacroID())
    return false;

  const SourceManager &SM = ACtx.getSourceManager();
  const LangOptions &LangOpts = ACtx.getLangOpts();

  return Lexer::getImmediateMacroName(Loc, SM, LangOpts) == Name;
}

static bool expressionMentionsMacro(const Stmt *S, StringRef Name,
                                    const ASTContext &ACtx) {
  if (!S)
    return false;

  if (isMacroNamed(S->getBeginLoc(), Name, ACtx) ||
      isMacroNamed(S->getEndLoc(), Name, ACtx))
    return true;

  for (const Stmt *Child : S->children()) {
    if (expressionMentionsMacro(Child, Name, ACtx))
      return true;
  }

  return false;
}

/// KEY_SIZE_MAX is a per-key representational bound, not a filesystem-wide
/// disk-sector count. Masking it by block_bits only rounds the bound down.
static bool isKeyEncodingBoundedSectorLimit(const VarDecl *VD,
                                            const ASTContext &ACtx) {
  if (!VD || !VD->hasInit())
    return false;

  const std::string Name = getLowerCaseName(VD);
  const bool IsMaxLike =
      Name == "max_sectors" ||
      Name == "sectors_max" ||
      Name.find("max_sector") != std::string::npos;

  if (!IsMaxLike)
    return false;

  const Expr *Init = VD->getInit();
  const StringRef InitText = getSourceText(Init->getSourceRange(), ACtx);

  const bool HasKeySizeBound =
      InitText.contains("KEY_SIZE_MAX") ||
      expressionMentionsMacro(Init, "KEY_SIZE_MAX", ACtx);

  return HasKeySizeBound &&
         InitText.contains("block_bits") &&
         InitText.contains("&");
}

static bool isSectorExpression(const Expr *E) {
  if (!E)
    return false;

  E = E->IgnoreParenImpCasts();

  if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
    return hasSectorName(DRE->getDecl());

  if (const auto *ME = dyn_cast<MemberExpr>(E))
    return hasSectorName(ME->getMemberDecl());

  return false;
}

static bool isDigit(char Ch) {
  return Ch >= '0' && Ch <= '9';
}

/// Returns true only when an unqualified "%u" conversion consumes the
/// requested call argument. It handles ordinary sequential conversions and
/// the common positional form "%N$u".
static bool hasUnsignedIntFormatForArgument(StringRef Format,
                                            unsigned FirstVariadicArg,
                                            unsigned WantedArg) {
  unsigned NextArg = FirstVariadicArg;

  for (size_t Pos = 0; Pos < Format.size(); ++Pos) {
    if (Format[Pos] != '%')
      continue;

    ++Pos;
    if (Pos >= Format.size())
      break;

    if (Format[Pos] == '%')
      continue;

    unsigned ArgumentIndex = NextArg;
    bool UsesPositionalArgument = false;

    size_t NumberStart = Pos;
    while (Pos < Format.size() && isDigit(Format[Pos]))
      ++Pos;

    if (Pos < Format.size() && Pos != NumberStart && Format[Pos] == '$') {
      unsigned Position = 0;
      for (size_t I = NumberStart; I < Pos; ++I)
        Position = Position * 10 + (Format[I] - '0');

      if (Position != 0) {
        ArgumentIndex = FirstVariadicArg + Position - 1;
        UsesPositionalArgument = true;
      }
      ++Pos;
    } else {
      Pos = NumberStart;
    }

    while (Pos < Format.size()) {
      const char Ch = Format[Pos];
      if (Ch != '#' && Ch != '0' && Ch != '-' && Ch != ' ' &&
          Ch != '+' && Ch != '\'')
        break;
      ++Pos;
    }

    if (Pos < Format.size() && Format[Pos] == '*') {
      ++Pos;
      if (!UsesPositionalArgument)
        ++NextArg;

      while (Pos < Format.size() && isDigit(Format[Pos]))
        ++Pos;
      if (Pos < Format.size() && Format[Pos] == '$')
        ++Pos;
    } else {
      while (Pos < Format.size() && isDigit(Format[Pos]))
        ++Pos;
    }

    if (Pos < Format.size() && Format[Pos] == '.') {
      ++Pos;
      if (Pos < Format.size() && Format[Pos] == '*') {
        ++Pos;
        if (!UsesPositionalArgument)
          ++NextArg;

        while (Pos < Format.size() && isDigit(Format[Pos]))
          ++Pos;
        if (Pos < Format.size() && Format[Pos] == '$')
          ++Pos;
      } else {
        while (Pos < Format.size() && isDigit(Format[Pos]))
          ++Pos;
      }
    }

    StringRef LengthModifier;
    if (Pos + 1 < Format.size() &&
        ((Format[Pos] == 'h' && Format[Pos + 1] == 'h') ||
         (Format[Pos] == 'l' && Format[Pos + 1] == 'l'))) {
      LengthModifier = Format.substr(Pos, 2);
      Pos += 2;
    } else if (Pos < Format.size() &&
               (Format[Pos] == 'h' || Format[Pos] == 'l' ||
                Format[Pos] == 'j' || Format[Pos] == 'z' ||
                Format[Pos] == 't' || Format[Pos] == 'L')) {
      LengthModifier = Format.substr(Pos, 1);
      ++Pos;
    }

    if (Pos >= Format.size())
      break;

    const char Conversion = Format[Pos];

    if (Conversion == 'u' && LengthModifier.empty() &&
        ArgumentIndex == WantedArg)
      return true;

    // '%' consumes no argument. GNU '%m' also consumes no argument.
    if (!UsesPositionalArgument && Conversion != '%' && Conversion != 'm')
      ++NextArg;
  }

  return false;
}

class SectorVarVisitor : public RecursiveASTVisitor<SectorVarVisitor> {
  struct Candidate {
    const VarDecl *Decl;
    bool HasNarrowingInitializer;
    bool HasWideUse;
  };

  ASTContext &ACtx;
  BugReporter &BR;
  const BugType &DeclBugTy;
  llvm::SmallVector<Candidate, 8> Candidates;

  Candidate *findCandidate(const VarDecl *VD) {
    for (Candidate &C : Candidates) {
      if (C.Decl == VD)
        return &C;
    }
    return nullptr;
  }

  bool hasRiskyUse(const DeclRefExpr *DRE, bool IsParameter) const {
    DynTypedNode Child = DynTypedNode::create(*DRE);
    const Stmt *ChildStmt = DRE;

    for (unsigned Depth = 0; Depth != 12; ++Depth) {
      auto Parents = ACtx.getParents(Child);
      if (Parents.size() != 1)
        break;

      const DynTypedNode &Parent = Parents[0];

      if (const auto *ICE = Parent.get<ImplicitCastExpr>()) {
        if (isWideIntegerType(ICE->getType(), ACtx))
          return true;
      }

      if (const auto *BO = Parent.get<BinaryOperator>()) {
        const bool IsLHS = BO->getLHS() == ChildStmt;
        const bool IsRHS = BO->getRHS() == ChildStmt;

        if (BO->isAssignmentOp()) {
          // A parameter assigned from a u64 expression means that its API
          // boundary truncates a value the function itself treats as wide.
          if (IsParameter && IsLHS &&
              isWideSourceExpression(BO->getRHS(), ACtx))
            return true;
        } else if (IsLHS || IsRHS) {
          const Expr *Other = IsLHS ? BO->getRHS() : BO->getLHS();
          if (isWideIntegerType(Other->getType(), ACtx))
            return true;
        }
      }

      if (const auto *CE = Parent.get<CallExpr>()) {
        const FunctionDecl *Callee = CE->getDirectCallee();

        if (Callee) {
          for (unsigned I = 0; I < CE->getNumArgs(); ++I) {
            if (CE->getArg(I) != ChildStmt)
              continue;

            if (I < Callee->getNumParams() &&
                isWideIntegerType(Callee->getParamDecl(I)->getType(), ACtx))
              return true;
          }
        }
      }

      const Stmt *ParentStmt = Parent.get<Stmt>();
      if (!ParentStmt)
        break;

      Child = Parent;
      ChildStmt = ParentStmt;
    }

    return false;
  }

public:
  SectorVarVisitor(ASTContext &ACtx, BugReporter &BR, const BugType &DeclBugTy)
      : ACtx(ACtx), BR(BR), DeclBugTy(DeclBugTy) {}

  void addCandidate(const VarDecl *VD) {
    if (!VD || !VD->getIdentifier() || !hasSectorName(VD) ||
        !isUnsignedIntType(VD->getType()))
      return;

    if (findCandidate(VD))
      return;

    const bool HasNarrowingInitializer =
        VD->hasInit() && isWideSourceExpression(VD->getInit(), ACtx);

    Candidates.push_back(
        {VD, HasNarrowingInitializer, false});
  }

  bool VisitVarDecl(VarDecl *VD) {
    // Parameters are explicitly registered from FunctionDecl::parameters().
    if (!isa<ParmVarDecl>(VD))
      addCandidate(VD);

    return true;
  }

  bool VisitDeclRefExpr(DeclRefExpr *DRE) {
    const auto *VD = dyn_cast<VarDecl>(DRE->getDecl());
    if (!VD)
      return true;

    Candidate *C = findCandidate(VD);
    if (!C || C->HasWideUse)
      return true;

    C->HasWideUse = hasRiskyUse(DRE, isa<ParmVarDecl>(VD));
    return true;
  }

  void reportFindings() {
    for (const Candidate &C : Candidates) {
      if (!C.HasNarrowingInitializer && !C.HasWideUse)
        continue;

      if (isKeyEncodingBoundedSectorLimit(C.Decl, ACtx))
        continue;

      SmallString<160> Buffer;
      llvm::raw_svector_ostream OS(Buffer);
      OS << "Variable '" << C.Decl->getNameAsString()
         << "' narrows a disk-sector value that is used as a wider integer "
            "(use u64 instead of unsigned int)";

      PathDiagnosticLocation Loc = PathDiagnosticLocation::createBegin(
          C.Decl, BR.getSourceManager());

      auto Report = std::make_unique<BasicBugReport>(DeclBugTy, OS.str(), Loc);
      Report->addRange(C.Decl->getSourceRange());
      BR.emitReport(std::move(Report));
    }
  }
};

class SAGenTestChecker : public Checker<check::ASTCodeBody, check::PostCall> {
  mutable std::unique_ptr<BugType> DeclBugTy;
  mutable std::unique_ptr<BugType> FormatBugTy;

public:
  SAGenTestChecker() {
    DeclBugTy = std::make_unique<BugType>(
        this, "Disk sector count variable width", "Integer Overflow");
    FormatBugTy = std::make_unique<BugType>(
        this, "Mismatched format specifier for disk sectors",
        "Integer Overflow");
  }

  void checkASTCodeBody(const Decl *D, AnalysisManager &Mgr,
                        BugReporter &BR) const {
    const auto *FD = dyn_cast<FunctionDecl>(D);
    if (!FD || !FD->hasBody())
      return;

    ASTContext &ACtx = Mgr.getASTContext();
    SectorVarVisitor Visitor(ACtx, BR, *DeclBugTy);

    // Traversing only the body does not visit FunctionDecl parameters.
    for (const ParmVarDecl *PVD : FD->parameters())
      Visitor.addCandidate(PVD);

    Visitor.TraverseStmt(FD->getBody());
    Visitor.reportFindings();
  }

  void checkPostCall(const CallEvent &Call, CheckerContext &C) const {
    const IdentifierInfo *Callee = Call.getCalleeIdentifier();
    if (!Callee || Callee->getName() != "bch2_trans_inconsistent")
      return;

    for (unsigned FormatArgIndex = 0; FormatArgIndex < Call.getNumArgs();
         ++FormatArgIndex) {
      const Expr *FormatArg = Call.getArgExpr(FormatArgIndex);
      if (!FormatArg)
        continue;

      FormatArg = FormatArg->IgnoreParenImpCasts();
      const auto *SL = dyn_cast<StringLiteral>(FormatArg);
      if (!SL)
        continue;

      const StringRef Format = SL->getString();
      const unsigned FirstVariadicArg = FormatArgIndex + 1;

      for (unsigned ArgIndex = FirstVariadicArg;
           ArgIndex < Call.getNumArgs(); ++ArgIndex) {
        const Expr *ValueArg = Call.getArgExpr(ArgIndex);
        if (!ValueArg || !isSectorExpression(ValueArg) ||
            !isWideIntegerType(ValueArg->getType(), C.getASTContext()))
          continue;

        if (!hasUnsignedIntFormatForArgument(
                Format, FirstVariadicArg, ArgIndex))
          continue;

        ExplodedNode *N = C.generateNonFatalErrorNode();
        if (!N)
          return;

        auto Report = std::make_unique<PathSensitiveBugReport>(
            *FormatBugTy,
            "64-bit disk-sector value is printed with %u; use %llu",
            N);
        Report->addRange(ValueArg->getSourceRange());
        C.emitReport(std::move(Report));
        return;
      }

      return;
    }
  }
};

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Checks for disk-sector values narrowed from 64-bit arithmetic and "
      "mismatched disk-sector format specifiers",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
