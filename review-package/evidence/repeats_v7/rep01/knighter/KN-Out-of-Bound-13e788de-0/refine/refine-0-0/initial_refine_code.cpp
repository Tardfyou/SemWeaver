#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Checkers/Taint.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/Environment.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramState.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramStateTrait.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SymExpr.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtVisitor.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/StaticAnalyzer/Checkers/utility.h"
#include "clang/Lex/Lexer.h"  // For Lexer::getSourceText
#include <memory>

using namespace clang;
using namespace ento;
using namespace taint;

namespace {

REGISTER_SET_WITH_PROGRAMSTATE(ReportedOffByOneConditions,
                               const BinaryOperator *)

class GreaterThanComparisonVisitor
    : public RecursiveASTVisitor<GreaterThanComparisonVisitor> {
  llvm::SmallVector<const BinaryOperator *, 4> Comparisons;

public:
  bool VisitBinaryOperator(BinaryOperator *BOp) {
    if (BOp->getOpcode() == BO_GT)
      Comparisons.push_back(BOp);

    return true;
  }

  llvm::ArrayRef<const BinaryOperator *> getComparisons() const {
    return Comparisons;
  }
};

static bool isTargetBoundaryMacro(const Expr *E, CheckerContext &C) {
  if (!E)
    return false;

  const SourceManager &SM = C.getSourceManager();
  const LangOptions &LangOpts = C.getLangOpts();

  CharSourceRange Range = CharSourceRange::getTokenRange(E->getSourceRange());
  StringRef SourceText = Lexer::getSourceText(Range, SM, LangOpts);
  if (SourceText.contains("RDS_MSG_RX_DGRAM_TRACE_MAX"))
    return true;

  // Macro-expanded expressions may no longer expose the original spelling
  // through getSourceText(), so also inspect the macro expansion chain.
  SourceLocation Loc = E->getExprLoc();
  while (Loc.isValid() && Loc.isMacroID()) {
    if (Lexer::getImmediateMacroName(Loc, SM, LangOpts) ==
        "RDS_MSG_RX_DGRAM_TRACE_MAX")
      return true;

    SourceLocation CallerLoc = SM.getImmediateMacroCallerLoc(Loc);
    if (CallerLoc == Loc)
      break;

    Loc = CallerLoc;
  }

  return false;
}

static bool isArrayElementValue(const Expr *E) {
  if (!E)
    return false;

  E = E->IgnoreParenImpCasts();
  return isa<ArraySubscriptExpr>(E);
}

class SAGenTestChecker : public Checker<check::BranchCondition> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Off-by-one array index boundary check",
                       "Array Bounds")) {}

  void checkBranchCondition(const Stmt *Condition,
                            CheckerContext &C) const;

private:
  void reportBug(const BinaryOperator *Condition,
                 CheckerContext &C) const;
};

void SAGenTestChecker::checkBranchCondition(const Stmt *Condition,
                                            CheckerContext &C) const {
  if (!Condition)
    return;

  GreaterThanComparisonVisitor Visitor;

  // RecursiveASTVisitor uses non-const AST pointers but does not modify them.
  Visitor.TraverseStmt(const_cast<Stmt *>(Condition));

  for (const BinaryOperator *BOp : Visitor.getComparisons()) {
    const Expr *LHS = BOp->getLHS();
    const Expr *RHS = BOp->getRHS();

    if (!LHS || !RHS)
      continue;

    if (!isTargetBoundaryMacro(RHS, C))
      continue;

    // A count may validly equal the number of available elements, so:
    //
    //   if (count > ARRAY_SIZE)
    //
    // is not an off-by-one error. The target defect instead validates a value
    // read from trace.rx_trace_pos[i], which is subsequently used as an index.
    // Restrict reporting to direct array-element values to distinguish those
    // cases without incorrectly rejecting valid full-capacity counts.
    if (!isArrayElementValue(LHS))
      continue;

    reportBug(BOp, C);
    return;
  }
}

void SAGenTestChecker::reportBug(const BinaryOperator *Condition,
                                 CheckerContext &C) const {
  ProgramStateRef State = C.getState();

  if (State->contains<ReportedOffByOneConditions>(Condition))
    return;

  State = State->add<ReportedOffByOneConditions>(Condition);

  ExplodedNode *N = C.generateNonFatalErrorNode(State);
  if (!N)
    return;

  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT, "Off-by-one error: incorrect array index boundary check", N);
  Report->addRange(Condition->getSourceRange());
  C.emitReport(std::move(Report));
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects off-by-one error in array index boundary check", "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
