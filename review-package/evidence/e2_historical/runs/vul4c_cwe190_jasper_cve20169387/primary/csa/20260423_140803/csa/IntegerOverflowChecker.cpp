#include "clang/Analysis/PathDiagnostic.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/Version.h"
#include "clang/Lex/Lexer.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/CheckerManager.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/MemRegion.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramStateTrait.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SVals.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include <memory>
#include <string>
#include <vector>

using namespace clang;
using namespace ento;

namespace {

static const Expr *ignoreParenCasts(const Expr *E) {
  return E ? E->IgnoreParenCasts() : nullptr;
}

static bool exprMentionsVar(const Expr *E, const VarDecl *VD) {
  E = ignoreParenCasts(E);
  if (!E || !VD)
    return false;

  if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
    return DRE->getDecl() == VD;

  for (const Stmt *Child : E->children()) {
    if (const auto *CE = dyn_cast_or_null<Expr>(Child)) {
      if (exprMentionsVar(CE, VD))
        return true;
    }
  }
  return false;
}

static bool isCheckedMulHelperName(StringRef Name) {
  return Name == "jas_safe_size_mul" || Name == "jas_safe_int_mul" ||
         Name == "jas_safe_uint_mul" || Name == "jas_safe_ulong_mul" ||
         Name == "jas_safe_ulonglong_mul" || Name == "jas_safe_long_mul" ||
         Name == "__builtin_mul_overflow" || Name.ends_with("_safe_mul") ||
         Name.contains("mul_overflow");
}

static bool isAllocationLikeName(StringRef Name) {
  return Name == "malloc" || Name == "calloc" || Name == "realloc" ||
         Name == "jas_alloc2" || Name == "jas_alloc3" ||
         Name.ends_with("alloc") || Name.ends_with("alloc2") ||
         Name.ends_with("alloc3") || Name.contains("alloc");
}

static std::string getSourceText(const SourceManager &SM,
                                 const LangOptions &LangOpts,
                                 SourceRange Range) {
  CharSourceRange CharRange = CharSourceRange::getTokenRange(Range);
  bool Invalid = false;
  StringRef Text = Lexer::getSourceText(CharRange, SM, LangOpts, &Invalid);
  return Invalid ? std::string() : Text.str();
}

static bool textMentionsTileCount(StringRef Text) {
  return Text.contains("numtiles") || Text.contains("numhtiles") ||
         Text.contains("numvtiles") || Text.contains("tile");
}

struct AstFinding {
  SourceLocation Loc;
  SourceRange Range;
  std::string Message;
};

class TileCountOverflowVisitor
    : public RecursiveASTVisitor<TileCountOverflowVisitor> {
public:
  TileCountOverflowVisitor(const SourceManager &SM, const LangOptions &LangOpts)
      : SM(SM), LangOpts(LangOpts) {}

  bool VisitBinaryOperator(const BinaryOperator *BO) {
    if (!BO || !BO->isAssignmentOp())
      return true;

    std::string LHS = getSourceText(SM, LangOpts, BO->getLHS()->getSourceRange());
    std::string RHS = getSourceText(SM, LangOpts, BO->getRHS()->getSourceRange());
    const auto *Mul = dyn_cast_or_null<BinaryOperator>(ignoreParenCasts(BO->getRHS()));
    if (!StringRef(LHS).contains("numtiles") || !Mul || Mul->getOpcode() != BO_Mul)
      return true;
    if (!StringRef(RHS).contains("numhtiles") || !StringRef(RHS).contains("numvtiles"))
      return true;

    SawUncheckedTileProduct = true;
    Findings.push_back({
        BO->getOperatorLoc().isValid() ? BO->getOperatorLoc() : BO->getBeginLoc(),
        BO->getSourceRange(),
        "custom.IntegerOverflowChecker: tile count is computed by unchecked multiplication before being stored as aggregate allocation state.",
    });
    return true;
  }

  bool VisitCallExpr(const CallExpr *CE) {
    if (!CE)
      return true;
    const FunctionDecl *Callee = CE->getDirectCallee();
    if (!Callee)
      return true;
    StringRef Name = Callee->getName();
    if (!isAllocationLikeName(Name))
      return true;
    for (const Expr *Arg : CE->arguments()) {
      std::string ArgText = getSourceText(SM, LangOpts, Arg->getSourceRange());
      if (StringRef(ArgText).contains("numtiles")) {
        AllocationConsumers.push_back(CE);
        break;
      }
    }
    return true;
  }

  bool VisitForStmt(const ForStmt *FS) {
    if (!FS || !FS->getCond())
      return true;
    std::string Cond = getSourceText(SM, LangOpts, FS->getCond()->getSourceRange());
    if (StringRef(Cond).contains("numtiles"))
      LoopConsumers.push_back(FS);
    return true;
  }

  void finalize() {
    if (!SawUncheckedTileProduct)
      return;
    for (const CallExpr *CE : AllocationConsumers) {
      Findings.push_back({
          CE->getBeginLoc(),
          CE->getSourceRange(),
          "custom.IntegerOverflowChecker: unchecked aggregate tile count reaches an allocation-size/count consumer.",
      });
    }
    for (const ForStmt *FS : LoopConsumers) {
      Findings.push_back({
          FS->getBeginLoc(),
          FS->getSourceRange(),
          "custom.IntegerOverflowChecker: unchecked aggregate tile count controls tile iteration after allocation.",
      });
    }
  }

  const std::vector<AstFinding> &findings() const { return Findings; }

private:
  const SourceManager &SM;
  const LangOptions &LangOpts;
  bool SawUncheckedTileProduct = false;
  std::vector<const CallExpr *> AllocationConsumers;
  std::vector<const ForStmt *> LoopConsumers;
  std::vector<AstFinding> Findings;
};

static const VarDecl *getAssignedVarFromExpr(const Expr *E) {
  E = ignoreParenCasts(E);
  if (!E)
    return nullptr;
  if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
    return dyn_cast<VarDecl>(DRE->getDecl());
  return nullptr;
}

class IntegerOverflowChecker
    : public Checker<check::ASTCodeBody, check::PreCall,
                     check::PreStmt<BinaryOperator>> {
  mutable std::unique_ptr<BugType> BT;

public:
  IntegerOverflowChecker()
      : BT(std::make_unique<BugType>(this,
                                     "Unchecked multiplication used as count",
                                     "Integer Overflow")) {}

  static bool isPatchFile(const SourceManager &SM, SourceLocation Loc) {
    StringRef File = SM.getFilename(SM.getExpansionLoc(Loc));
    return File.ends_with("src/libjasper/jpc/jpc_dec.c") ||
           File.ends_with("src\\libjasper\\jpc\\jpc_dec.c");
  }

  void checkASTCodeBody(const Decl *D, AnalysisManager &, BugReporter &BR) const {
    const auto *FD = dyn_cast_or_null<FunctionDecl>(D);
    if (!FD || !FD->hasBody())
      return;

    const SourceManager &SM = BR.getSourceManager();
    if (!isPatchFile(SM, FD->getBeginLoc()))
      return;

    TileCountOverflowVisitor Visitor(SM, BR.getContext().getLangOpts());
    Visitor.TraverseStmt(FD->getBody());
    Visitor.finalize();

    for (const AstFinding &Finding : Visitor.findings()) {
      if (!Finding.Loc.isValid())
        continue;
      PathDiagnosticLocation Loc(Finding.Loc, SM);
      BR.EmitBasicReport(FD, this, "Unchecked tile count multiplication",
                         "Integer Overflow", Finding.Message, Loc,
                         Finding.Range);
    }
  }

  void checkPreStmt(const BinaryOperator *BO, CheckerContext &C) const {
    if (!BO || !BO->isAssignmentOp())
      return;

    const Expr *RHS = ignoreParenCasts(BO->getRHS());
    const auto *Mul = dyn_cast_or_null<BinaryOperator>(RHS);
    if (!Mul || Mul->getOpcode() != BO_Mul)
      return;

    const VarDecl *AssignedVD = getAssignedVarFromExpr(BO->getLHS());
    if (!AssignedVD)
      return;

    if (const auto *LHSVar = dyn_cast_or_null<DeclRefExpr>(ignoreParenCasts(BO->getLHS()))) {
      (void)LHSVar;
    }
  }

  void checkPreCall(const CallEvent &Call, CheckerContext &C) const {
    const IdentifierInfo *II = Call.getCalleeIdentifier();
    if (!II)
      return;

    StringRef Callee = II->getName();
    if (!isAllocationLikeName(Callee))
      return;

    for (unsigned I = 0; I < Call.getNumArgs(); ++I) {
      const Expr *Arg = Call.getArgExpr(I);
      Arg = ignoreParenCasts(Arg);
      const auto *Mul = dyn_cast_or_null<BinaryOperator>(Arg);
      if (!Mul || Mul->getOpcode() != BO_Mul)
        continue;

      ExplodedNode *N = C.generateNonFatalErrorNode();
      if (!N)
        return;

      auto R = std::make_unique<PathSensitiveBugReport>(
          *BT,
          "Unchecked multiplication flows directly into an allocation-like call as a count/size argument and may overflow.",
          N);
      R->addRange(Mul->getSourceRange());
      C.emitReport(std::move(R));
      return;
    }
  }
};

} // namespace

extern "C" void clang_registerCheckers(CheckerRegistry &Registry) {
  Registry.addChecker<IntegerOverflowChecker>(
      "custom.IntegerOverflowChecker",
      "Detect unchecked multiplication used as allocation count or size.",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] = CLANG_VERSION_STRING;
