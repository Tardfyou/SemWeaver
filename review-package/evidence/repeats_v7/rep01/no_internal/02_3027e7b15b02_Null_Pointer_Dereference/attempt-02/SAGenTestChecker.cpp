// /external/upstream-study/SAGEN/result-0224-bugfail-multi-o3mini/test-Null-Pointer-Dereference-3027e7b15b02d2d37e3f82d6b8404f6d37e3b8cf/checkers/checker2.cpp
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
#include "clang/AST/ParentMapContext.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/StaticAnalyzer/Checkers/utility.h"

// Additional necessary includes
#include "clang/Lex/Lexer.h"
#include "clang/Basic/LangOptions.h"

using namespace clang;
using namespace ento;
using namespace taint;

// Tracks regions whose values originate from a fallible devm_kasprintf call.
REGISTER_MAP_WITH_PROGRAMSTATE(PotentiallyNullPtrMap, const MemRegion*, bool)

namespace {

static bool isNullGuardOperand(const Stmt *S, ASTContext &Ctx) {
  const Stmt *Current = S;
  while (Current) {
    auto Parents = Ctx.getParents(*Current);
    if (Parents.empty())
      return false;

    const Stmt *Parent = Parents[0].get<Stmt>();
    if (!Parent)
      return false;

    if (isa<ParenExpr>(Parent) || isa<ImplicitCastExpr>(Parent) ||
        isa<CStyleCastExpr>(Parent)) {
      Current = Parent;
      continue;
    }

    if (const auto *UO = dyn_cast<UnaryOperator>(Parent))
      return UO->getOpcode() == UO_LNot;

    if (const auto *BO = dyn_cast<BinaryOperator>(Parent)) {
      if (BO->getOpcode() != BO_EQ && BO->getOpcode() != BO_NE)
        return false;
      const Expr *Other = BO->getLHS() == Current ? BO->getRHS()
                                                   : BO->getLHS();
      return Other->IgnoreParenImpCasts()->isNullPointerConstant(
          Ctx, Expr::NPC_ValueDependentIsNull);
    }

    if (const auto *IS = dyn_cast<IfStmt>(Parent))
      return IS->getCond() == Current;
    if (const auto *WS = dyn_cast<WhileStmt>(Parent))
      return WS->getCond() == Current;
    if (const auto *FS = dyn_cast<ForStmt>(Parent))
      return FS->getCond() == Current;
    if (const auto *DS = dyn_cast<DoStmt>(Parent))
      return DS->getCond() == Current;

    return false;
  }
  return false;
}

class SAGenTestChecker : public Checker<
    check::PostCall, // For intercepting devm_kasprintf return values.
    check::Bind,     // For propagating the pointer's fallibility state.
    check::Location  // For catching potentially-null pointer uses.
    > {
  mutable std::unique_ptr<BugType> BT;
  
public:
  SAGenTestChecker() : BT(new BugType(this, "Unchecked devm_kasprintf return", "Null Dereference")) {}

  // Callback: After a function call is evaluated.
  void checkPostCall(const CallEvent &Call, CheckerContext &C) const;

  // Callback: When an assignment (binding) occurs.
  void checkBind(SVal Loc, SVal Val, const Stmt *StoreE, CheckerContext &C) const;

  // Callback: When a memory location is accessed (load or store).
  void checkLocation(SVal Loc, bool IsLoad, const Stmt *S, CheckerContext &C) const;

private:
  // Helper for reporting the bug.
  void reportUncheckedUse(const Stmt *S, CheckerContext &C) const;
};

/// checkPostCall - Intercept calls to devm_kasprintf. If the function is called,
/// mark its returned memory region as "unchecked" (false) in PossibleNullPtrMap.
void SAGenTestChecker::checkPostCall(const CallEvent &Call, CheckerContext &C) const {
  const Expr *OriginExpr = Call.getOriginExpr();
  if (!OriginExpr)
    return;
  
  // Use the utility function to check if this call is to devm_kasprintf.
  if (!ExprHasName(OriginExpr, "devm_kasprintf", C))
    return;
  
  // Retrieve the memory region for the returned pointer.
  const MemRegion *MR = getMemRegionFromExpr(OriginExpr, C);
  if (!MR)
    return;
  MR = MR->getBaseRegion();
  if (!MR)
    return;
  
  ProgramStateRef State = C.getState();
  State = State->set<PotentiallyNullPtrMap>(MR, true);
  C.addTransition(State);
}

/// checkBind - Propagate the fallible-result origin across pointer assignments.
void SAGenTestChecker::checkBind(SVal Loc, SVal Val, const Stmt *StoreE,
                                 CheckerContext &C) const {
  ProgramStateRef State = C.getState();
  const MemRegion *LHSReg = Loc.getAsRegion();
  const MemRegion *RHSReg = Val.getAsRegion();
  if (!LHSReg || !RHSReg)
    return;

  LHSReg = LHSReg->getBaseRegion();
  RHSReg = RHSReg->getBaseRegion();
  if (!LHSReg || !RHSReg)
    return;

  if (!State->get<PotentiallyNullPtrMap>(RHSReg))
    return;

  State = State->set<PotentiallyNullPtrMap>(LHSReg, true);
  C.addTransition(State);
}

/// checkLocation - Report a use only when the tracked value may still be NULL.
void SAGenTestChecker::checkLocation(SVal Loc, bool IsLoad, const Stmt *S,
                                     CheckerContext &C) const {
  const MemRegion *MR = Loc.getAsRegion();
  if (!MR || isNullGuardOperand(S, C.getASTContext()))
    return;

  MR = MR->getBaseRegion();
  if (!MR)
    return;

  ProgramStateRef State = C.getState();
  if (!State->get<PotentiallyNullPtrMap>(MR))
    return;

  SVal PointerValue = State->getSVal(MR, C.getLocationContext());
  ProgramStateRef NullState = State->assume(PointerValue, false);
  if (NullState)
    reportUncheckedUse(S, C);
}

/// reportUncheckedUse - Generate a bug report when an unchecked devm_kasprintf
/// pointer is dereferenced.
void SAGenTestChecker::reportUncheckedUse(const Stmt *S, CheckerContext &C) const {
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;
  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT, "Unchecked devm_kasprintf return value used", N);
  Report->addRange(S->getSourceRange());
  C.emitReport(std::move(Report));
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker", 
      "Detects use of devm_kasprintf return value without NULL checking",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
