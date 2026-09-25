// KNighter E2 case 07: detect the pre-fix cleanup edge that frees mt->fc
// after hws_definer_conv_match_params_to_hl() fails.
#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/AnalysisManager.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include "llvm/ADT/StringRef.h"
#include <memory>

using namespace clang;
using namespace ento;

namespace {

static std::string sourceText(const Stmt *S, ASTContext &Ctx) {
  if (!S)
    return "";
  SourceManager &SM = Ctx.getSourceManager();
  SourceRange Range = S->getSourceRange();
  if (Range.isInvalid())
    return "";
  CharSourceRange CharRange = CharSourceRange::getTokenRange(Range);
  return Lexer::getSourceText(CharRange, SM, Ctx.getLangOpts()).str();
}

static bool callsFunction(const Expr *E, StringRef Name, ASTContext &Ctx) {
  if (!E)
    return false;
  std::string Text = sourceText(E, Ctx);
  return StringRef(Text).contains(Name);
}

static bool gotoLabel(const Stmt *S, StringRef LabelName) {
  const auto *GS = dyn_cast_or_null<GotoStmt>(S);
  return GS && GS->getLabel() && GS->getLabel()->getName() == LabelName;
}

class ConvFailureCleanupVisitor
    : public RecursiveASTVisitor<ConvFailureCleanupVisitor> {
  ASTContext &Ctx;
  const IfStmt *BadCleanupIf = nullptr;

public:
  explicit ConvFailureCleanupVisitor(ASTContext &Ctx) : Ctx(Ctx) {}

  bool VisitIfStmt(IfStmt *IS) {
    if (BadCleanupIf)
      return false;

    const Stmt *Prev = previousNonNullSibling(IS);
    if (!isConvAssignment(Prev))
      return true;

    if (thenJumpsTo(IS, "free_fc"))
      BadCleanupIf = IS;
    return true;
  }

  const IfStmt *getBadCleanupIf() const { return BadCleanupIf; }

private:
  const Stmt *previousNonNullSibling(const Stmt *Target) const {
    auto Parents = Ctx.getParents(*Target);
    if (Parents.empty())
      return nullptr;

    const Stmt *ParentStmt = Parents.begin()->get<Stmt>();
    const auto *CS = dyn_cast_or_null<CompoundStmt>(ParentStmt);
    if (!CS)
      return nullptr;

    const Stmt *Previous = nullptr;
    for (const Stmt *Child : CS->body()) {
      if (Child == Target)
        return Previous;
      if (Child)
        Previous = Child;
    }
    return nullptr;
  }

  bool isConvAssignment(const Stmt *S) const {
    const auto *BO = dyn_cast_or_null<BinaryOperator>(S);
    if (!BO || !BO->isAssignmentOp())
      return false;
    std::string LHS = sourceText(BO->getLHS(), Ctx);
    if (StringRef(LHS).trim() != "ret")
      return false;
    return callsFunction(BO->getRHS(), "hws_definer_conv_match_params_to_hl",
                         Ctx);
  }

  bool thenJumpsTo(const IfStmt *IS, StringRef LabelName) const {
    const Stmt *Then = IS ? IS->getThen() : nullptr;
    if (!Then)
      return false;
    if (gotoLabel(Then, LabelName))
      return true;
    const auto *CS = dyn_cast<CompoundStmt>(Then);
    if (!CS)
      return false;
    for (const Stmt *Child : CS->body()) {
      if (gotoLabel(Child, LabelName))
        return true;
    }
    return false;
  }
};

class SAGenTestChecker : public Checker<check::ASTCodeBody> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(std::make_unique<BugType>(this,
                                     "Wrong cleanup target after conversion failure",
                                     "Memory Error")) {}

  void checkASTCodeBody(const Decl *D, AnalysisManager &Mgr,
                        BugReporter &BR) const {
    const auto *FD = dyn_cast<FunctionDecl>(D);
    if (!FD || FD->getNameAsString() != "mlx5hws_definer_calc_layout")
      return;
    const Stmt *Body = FD->getBody();
    if (!Body)
      return;

    ConvFailureCleanupVisitor Visitor(FD->getASTContext());
    Visitor.TraverseStmt(const_cast<Stmt *>(Body));
    const IfStmt *BadIf = Visitor.getBadCleanupIf();
    if (!BadIf)
      return;

    PathDiagnosticLocation Loc =
        PathDiagnosticLocation::createBegin(BadIf, BR.getSourceManager(),
                                            Mgr.getAnalysisDeclContext(D));
    auto Report = std::make_unique<BasicBugReport>(
        *BT,
        "Conversion failure jumps to free_fc and can release mt->fc before the "
        "field is established",
        Loc);
    Report->addRange(BadIf->getSourceRange());
    BR.emitReport(std::move(Report));
  }
};

} // namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detect wrong cleanup target after mlx5hws definer conversion failure",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
