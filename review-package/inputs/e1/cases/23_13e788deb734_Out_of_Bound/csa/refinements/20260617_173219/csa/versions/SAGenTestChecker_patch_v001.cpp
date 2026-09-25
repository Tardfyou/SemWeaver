// /external/upstream-study/SAGEN/result-0224-bugfail-multi-o3mini/test-Out-of-Bound-13e788deb7348cc88df34bed736c3b3b9927ea52/checkers/checker0.cpp
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
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

const Expr *stripExpr(const Expr *E) {
  return E ? E->IgnoreParenImpCasts() : nullptr;
}

std::string getExprText(const Expr *E, CheckerContext &C) {
  E = stripExpr(E);
  if (!E)
    return "";

  const SourceManager &SM = C.getSourceManager();
  CharSourceRange Range = CharSourceRange::getTokenRange(E->getSourceRange());
  return Lexer::getSourceText(Range, SM, C.getLangOpts()).str();
}

bool sameExprText(const Expr *LHS, const Expr *RHS, CheckerContext &C) {
  std::string LText = getExprText(LHS, C);
  std::string RText = getExprText(RHS, C);
  return !LText.empty() && LText == RText;
}

bool isArrayElementValue(const Expr *E) {
  return isa_and_nonnull<ArraySubscriptExpr>(stripExpr(E));
}

class ComparisonVisitor : public RecursiveASTVisitor<ComparisonVisitor> {
  CheckerContext &C;
  const BinaryOperator *Candidate = nullptr;
  const Expr *GuardedValue = nullptr;

public:
  explicit ComparisonVisitor(CheckerContext &C) : C(C) {}

  bool VisitBinaryOperator(const BinaryOperator *BOp) {
    if (Candidate || BOp->getOpcode() != BO_GT)
      return true;

    const Expr *LHS = stripExpr(BOp->getLHS());
    const Expr *RHS = stripExpr(BOp->getRHS());
    if (isArrayElementValue(LHS) && !isArrayElementValue(RHS)) {
      Candidate = BOp;
      GuardedValue = LHS;
    } else if (isArrayElementValue(RHS) && !isArrayElementValue(LHS)) {
      Candidate = BOp;
      GuardedValue = RHS;
    }
    return true;
  }

  const BinaryOperator *getCandidate() const { return Candidate; }
  const Expr *getGuardedValue() const { return GuardedValue; }
};

class AssignmentVisitor : public RecursiveASTVisitor<AssignmentVisitor> {
  CheckerContext &C;
  const Expr *GuardedValue;
  bool Found = false;

public:
  AssignmentVisitor(CheckerContext &C, const Expr *GuardedValue)
      : C(C), GuardedValue(GuardedValue) {}

  bool VisitBinaryOperator(const BinaryOperator *BOp) {
    if (Found || BOp->getOpcode() != BO_Assign)
      return true;

    const Expr *LHS = stripExpr(BOp->getLHS());
    const Expr *RHS = stripExpr(BOp->getRHS());
    if (isArrayElementValue(LHS) && sameExprText(RHS, GuardedValue, C))
      Found = true;
    return true;
  }

  bool foundCopy() const { return Found; }
};

class SAGenTestChecker : public Checker<check::BranchCondition> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Off-by-one guarded array value", "Array Bounds")) {}

  void checkBranchCondition(const Stmt *Condition, CheckerContext &C) const;

private:
  bool guardedValueIsCopiedAfterIf(const IfStmt *If, const Expr *GuardedValue,
                                   CheckerContext &C) const;
  void reportBug(const Stmt *Condition, CheckerContext &C) const;
};

bool SAGenTestChecker::guardedValueIsCopiedAfterIf(const IfStmt *If,
                                                   const Expr *GuardedValue,
                                                   CheckerContext &C) const {
  if (!If || !GuardedValue)
    return false;

  ASTContext &ACtx = C.getASTContext();
  DynTypedNodeList Parents = ACtx.getParents(*If);
  if (Parents.empty())
    return false;

  const auto *ParentBlock = Parents[0].get<CompoundStmt>();
  if (!ParentBlock)
    return false;

  bool SeenGuard = false;
  for (const Stmt *Child : ParentBlock->body()) {
    if (Child == If) {
      SeenGuard = true;
      continue;
    }
    if (!SeenGuard)
      continue;

    AssignmentVisitor Visitor(C, GuardedValue);
    Visitor.TraverseStmt(const_cast<Stmt *>(Child));
    if (Visitor.foundCopy())
      return true;
  }
  return false;
}

void SAGenTestChecker::checkBranchCondition(const Stmt *Condition,
                                            CheckerContext &C) const {
  if (!Condition)
    return;

  ComparisonVisitor Visitor(C);
  Visitor.TraverseStmt(const_cast<Stmt *>(Condition));
  const BinaryOperator *BOp = Visitor.getCandidate();
  const Expr *GuardedValue = Visitor.getGuardedValue();
  if (!BOp || !GuardedValue)
    return;

  const IfStmt *If = findSpecificTypeInParents<IfStmt>(Condition, C);
  if (!guardedValueIsCopiedAfterIf(If, GuardedValue, C))
    return;

  reportBug(BOp, C);
}

void SAGenTestChecker::reportBug(const Stmt *Condition, CheckerContext &C) const {
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT, "Off-by-one guard lets a boundary value flow into an array index",
      N);
  Report->addRange(Condition->getSourceRange());
  C.emitReport(std::move(Report));
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects off-by-one error in array index boundary check",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
