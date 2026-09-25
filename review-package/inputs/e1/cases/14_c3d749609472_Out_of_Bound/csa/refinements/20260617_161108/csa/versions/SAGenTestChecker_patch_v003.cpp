#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtVisitor.h"
#include "clang/Lex/Lexer.h"
#include "clang/StaticAnalyzer/Checkers/utility.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"

#include <memory>
#include <string>

using namespace clang;
using namespace ento;

namespace {

class SAGenTestChecker : public Checker<check::BranchCondition> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker() : BT(new BugType(this, "CRTC array loop bound mismatch")) {}

  void checkBranchCondition(const Stmt *Condition, CheckerContext &C) const;
};

static std::string getSourceText(const Expr *E, CheckerContext &C) {
  if (!E)
    return "";

  const SourceManager &SM = C.getSourceManager();
  SourceLocation Begin = E->getBeginLoc();
  SourceLocation End = E->getEndLoc();
  if (Begin.isInvalid() || End.isInvalid())
    return "";

  CharSourceRange Range = CharSourceRange::getTokenRange(Begin, End);
  return Lexer::getSourceText(Range, SM, C.getASTContext().getLangOpts()).str();
}

static const VarDecl *getReferencedVar(const Expr *E) {
  E = E ? E->IgnoreParenImpCasts() : nullptr;
  if (const auto *DRE = dyn_cast_or_null<DeclRefExpr>(E))
    return dyn_cast<VarDecl>(DRE->getDecl());
  return nullptr;
}

static bool isRiskyDisplayLinkBound(const Expr *E, CheckerContext &C) {
  std::string Text = getSourceText(E, C);
  if (Text.find("num_crtc") != std::string::npos)
    return false;

  return Text.find("max_links") != std::string::npos ||
         ExprHasName(E, "caps.max_links", C) ||
         ExprHasName(E, "max_links", C);
}

static bool isSecureDisplayContextStorage(const Expr *Base, CheckerContext &C) {
  const Expr *NormalizedBase = Base ? Base->IgnoreParenImpCasts() : nullptr;
  if (!NormalizedBase)
    return false;

  QualType BaseType = NormalizedBase->getType();
  if (const auto *PointerTy = BaseType->getAs<PointerType>())
    BaseType = PointerTy->getPointeeType();
  if (const auto *ArrayTy = C.getASTContext().getAsArrayType(BaseType))
    BaseType = ArrayTy->getElementType();
  if (const auto *RecordTy = BaseType->getAs<RecordType>())
    if (RecordTy->getDecl()->getName() == "secure_display_context")
      return true;

  std::string Text = getSourceText(Base, C);
  return Text.find("secure_display_ctxs") != std::string::npos ||
         ExprHasName(Base, "secure_display_ctxs", C);
}

class CrtcIndexedAccessVisitor
    : public RecursiveASTVisitor<CrtcIndexedAccessVisitor> {
  CheckerContext &C;
  const VarDecl *IndexVar;
  bool Found = false;

public:
  CrtcIndexedAccessVisitor(CheckerContext &C, const VarDecl *IndexVar)
      : C(C), IndexVar(IndexVar) {}

  bool VisitArraySubscriptExpr(ArraySubscriptExpr *ASE) {
    if (Found)
      return true;

    const VarDecl *UsedIndex = getReferencedVar(ASE->getIdx());
    if (UsedIndex && UsedIndex == IndexVar &&
        isSecureDisplayContextStorage(ASE->getBase(), C))
      Found = true;

    return true;
  }

  bool foundAccess() const { return Found; }
};

static bool extractsLoopIndexAndRiskyBound(const Expr *Cond, CheckerContext &C,
                                           const VarDecl *&IndexVar) {
  const auto *BO = dyn_cast<BinaryOperator>(Cond->IgnoreParenImpCasts());
  if (!BO || !BO->isRelationalOp())
    return false;

  const Expr *LHS = BO->getLHS();
  const Expr *RHS = BO->getRHS();
  BinaryOperatorKind Op = BO->getOpcode();

  if ((Op == BO_LT || Op == BO_LE) && isRiskyDisplayLinkBound(RHS, C)) {
    if (const VarDecl *LHSVar = getReferencedVar(LHS)) {
      IndexVar = LHSVar;
      return true;
    }
  }

  if ((Op == BO_GT || Op == BO_GE) && isRiskyDisplayLinkBound(LHS, C)) {
    if (const VarDecl *RHSVar = getReferencedVar(RHS)) {
      IndexVar = RHSVar;
      return true;
    }
  }

  return false;
}

void SAGenTestChecker::checkBranchCondition(const Stmt *Condition,
                                            CheckerContext &C) const {
  const Expr *CondExpr = dyn_cast<Expr>(Condition);
  if (!CondExpr)
    return;

  const ForStmt *FS = findSpecificTypeInParents<ForStmt>(Condition, C);
  if (!FS || FS->getCond() != Condition)
    return;

  const VarDecl *IndexVar = nullptr;
  if (!extractsLoopIndexAndRiskyBound(CondExpr, C, IndexVar) || !IndexVar)
    return;

  CrtcIndexedAccessVisitor Visitor(C, IndexVar);
  Visitor.TraverseStmt(const_cast<Stmt *>(FS->getBody()));
  if (!Visitor.foundAccess())
    return;

  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT,
      "Loop uses a display-link limit to index CRTC-sized storage; bound the loop by the CRTC capacity instead",
      N);
  Report->addRange(Condition->getSourceRange());
  C.emitReport(std::move(Report));
}

} // namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects loops that index CRTC-sized arrays with a wider display-link bound",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
