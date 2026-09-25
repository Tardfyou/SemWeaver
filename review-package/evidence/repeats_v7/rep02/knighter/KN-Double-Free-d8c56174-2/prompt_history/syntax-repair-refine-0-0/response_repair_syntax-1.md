```cpp
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
#include "llvm/Support/raw_ostream.h"
#include "clang/StaticAnalyzer/Checkers/utility.h"
#include "clang/AST/Expr.h" // for Expr

using namespace clang;
using namespace ento;
using namespace taint;

namespace clang {
namespace ento {

// Return values produced by hws_send_ring_set_sq_rdy().
REGISTER_SET_WITH_PROGRAMSTATE(SQReadyStatusSymbols, SymbolRef)

// Symbols known to be on the nonzero/error branch of an SQ-ready result.
REGISTER_SET_WITH_PROGRAMSTATE(ActiveSQReadyFailureSymbols, SymbolRef)

} // namespace ento
} // namespace clang

namespace {

static bool isCallNamed(const CallEvent &Call, llvm::StringRef Name) {
  const IdentifierInfo *II = Call.getCalleeIdentifier();
  return II && II->getName() == Name;
}

static bool containsStmt(const Stmt *Root, const Stmt *Needle) {
  if (!Root)
    return false;

  if (Root == Needle)
    return true;

  for (const Stmt *Child : Root->children()) {
    if (Child && containsStmt(Child, Needle))
      return true;
  }

  return false;
}

/// Finds the closest enclosing if-statement whose then/else branch contains S.
static const IfStmt *findEnclosingIfBranch(const Stmt *S, CheckerContext &C,
                                           bool &IsThenBranch) {
  const Stmt *Current = S;

  while (Current) {
    const DynTypedNodeList Parents =
        C.getASTContext().getParents(*Current);

    const Stmt *ParentStmt = nullptr;
    for (const DynTypedNode &Parent : Parents) {
      ParentStmt = Parent.get<Stmt>();
      if (ParentStmt)
        break;
    }

    if (!ParentStmt)
      return nullptr;

    if (const auto *If = dyn_cast<IfStmt>(ParentStmt)) {
      if (containsStmt(If->getThen(), S)) {
        IsThenBranch = true;
        return If;
      }

      if (containsStmt(If->getElse(), S)) {
        IsThenBranch = false;
        return If;
      }
    }

    Current = ParentStmt;
  }

  return nullptr;
}

/// Accept only direct C error checks such as "if (err)".
///
/// This intentionally excludes expressions such as "if (err == 0)", where the
/// then branch is a success path, and complex conditions whose error semantics
/// cannot be established reliably by this checker.
static SymbolRef getDirectConditionSymbol(const IfStmt *If,
                                          CheckerContext &C) {
  const Expr *Cond = If->getCond();
  if (!Cond)
    return nullptr;

  const Expr *StrippedCond = Cond->IgnoreParenImpCasts();
  if (!isa<DeclRefExpr>(StrippedCond))
    return nullptr;

  SVal CondValue = C.getState()->getSVal(Cond, C.getLocationContext());
  return CondValue.getAsSymbol();
}

// This checker detects the specific erroneous cleanup:
//   err = hws_send_ring_set_sq_rdy(...);
//   if (err)
//     hws_send_ring_close_sq(...);
//
// hws_send_ring_close_sq() is legitimate during normal queue shutdown, so a
// report requires a proven SQ-ready error path rather than just the callee name.
class SAGenTestChecker
    : public Checker<check::PreCall, check::PostCall, eval::Assume> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Double-free due to wrong cleanup function usage",
                       "Memory Error")) {}

  void checkPreCall(const CallEvent &Call, CheckerContext &C) const;
  void checkPostCall(const CallEvent &Call, CheckerContext &C) const;

  ProgramStateRef evalAssume(ProgramStateRef State, SVal Cond,
                             bool Assumption) const;
};

void SAGenTestChecker::checkPostCall(const CallEvent &Call,
                                     CheckerContext &C) const {
  if (!isCallNamed(Call, "hws_send_ring_set_sq_rdy"))
    return;

  SymbolRef ResultSymbol = Call.getReturnValue().getAsSymbol();
  if (!ResultSymbol)
    return;

  ProgramStateRef State = C.getState();
  if (State->contains<SQReadyStatusSymbols>(ResultSymbol))
    return;

  C.addTransition(State->add<SQReadyStatusSymbols>(ResultSymbol));
}

ProgramStateRef SAGenTestChecker::evalAssume(ProgramStateRef State, SVal Cond,
                                             bool Assumption) const {
  SymbolRef ConditionSymbol = Cond.getAsSymbol();
  if (!ConditionSymbol ||
      !State->contains<SQReadyStatusSymbols>(ConditionSymbol))
    return State;

  // For "if (err)", the true branch is the nonzero failure branch.
  if (Assumption)
    return State->add<ActiveSQReadyFailureSymbols>(ConditionSymbol);

  return State->remove<ActiveSQReadyFailureSymbols>(ConditionSymbol);
}

void SAGenTestChecker::checkPreCall(const CallEvent &Call,
                                    CheckerContext &C) const {
  if (!isCallNamed(Call, "hws_send_ring_close_sq"))
    return;

  const Expr *OriginExpr = Call.getOriginExpr();
  if (!OriginExpr)
    return;

  bool IsThenBranch = false;
  const IfStmt *ErrorIf =
      findEnclosingIfBranch(OriginExpr, C, IsThenBranch);

  // The target bug uses "if (err) hws_send_ring_close_sq(sq);".
  // A normal shutdown call has no corresponding error branch.
  if (!ErrorIf || !IsThenBranch)
    return;

  SymbolRef ConditionSymbol = getDirectConditionSymbol(ErrorIf, C);
  if (!ConditionSymbol)
    return;

  ProgramStateRef State = C.getState();
  if (!State->contains<SQReadyStatusSymbols>(ConditionSymbol) ||
      !State->contains<ActiveSQReadyFailureSymbols>(ConditionSymbol))
    return;

  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT,
      "Double-free error: hws_send_ring_close_sq is used after "
      "hws_send_ring_set_sq_rdy fails",
      N);
  Report->addRange(Call.getSourceRange());
  C.emitReport(std::move(Report));
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects erroneous SQ cleanup after hws_send_ring_set_sq_rdy failure",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
```