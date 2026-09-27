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
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/Twine.h"
#include "llvm/ADT/APSInt.h"
#include <memory>

using namespace clang;
using namespace ento;
using namespace taint;

// Program state map: the symbolic pointer value returned by a *_optional()
// getter may be NULL when the resource is absent. Track its null-check status:
//   0 = not NULL-checked on this path yet; 1 = NULL-checked.
// Keying on the value's SymbolRef keeps identity across assignment to a field
// or local and its later reload, so the guard and the dereference observe the
// same tracked pointer.
REGISTER_MAP_WITH_PROGRAMSTATE(OptionalPtrMap, SymbolRef, unsigned)

namespace {

class SAGenTestChecker
  : public Checker<
        check::PostCall,
        check::PreCall,
        check::BranchCondition,
        check::Location> {

   mutable std::unique_ptr<BugType> BT;

public:
   SAGenTestChecker()
     : BT(new BugType(this, "NULL dereference of *_optional() result", "API Misuse")) {}

   void checkPostCall(const CallEvent &Call, CheckerContext &C) const;
   void checkPreCall(const CallEvent &Call, CheckerContext &C) const;
   void checkBranchCondition(const Stmt *Condition, CheckerContext &C) const;
   void checkLocation(SVal Loc, bool IsLoad, const Stmt *S, CheckerContext &C) const;

private:
   // Helpers
   static bool isOptionalGetter(const CallEvent &Call, CheckerContext &C);

   // Identity of a pointer value: bare symbol or symbolic-region symbol.
   static SymbolRef symbolFromSVal(SVal V);
   // Symbol of the pointer denoted by expression E, if any.
   static SymbolRef getPtrSymbol(const Expr *E, CheckerContext &C);

   static bool exprIsNull(const Expr *E, CheckerContext &C);
   // Symbol of the pointer tested by a branch condition, if any.
   static SymbolRef extractPtrSymbolFromCond(const Expr *CondE,
                                             CheckerContext &C);

   void reportPossibleNullDeref(const Stmt *S, CheckerContext &C,
                                StringRef Extra = "") const;
};

bool SAGenTestChecker::isOptionalGetter(const CallEvent &Call, CheckerContext &C) {
  const Expr *Origin = Call.getOriginExpr();
  if (!Origin)
    return false;

  // Check that callee name contains "_optional"
  if (!ExprHasName(Origin, "_optional", C))
    return false;

  // Ensure it returns a pointer type
  QualType RTy = Call.getResultType();
  if (RTy.isNull() || !RTy->isPointerType())
    return false;

  return true;
}

SymbolRef SAGenTestChecker::symbolFromSVal(SVal V) {
  // A pointer value is either a bare symbol or a MemRegionVal whose region is
  // a SymbolicRegion; both identify the same SymbolRef across the store.
  if (SymbolRef Sym = V.getAsSymbol())
    return Sym;
  if (auto MRV = V.getAs<loc::MemRegionVal>())
    if (const auto *SR = dyn_cast<SymbolicRegion>(MRV->getRegion()))
      return SR->getSymbol();
  return nullptr;
}

SymbolRef SAGenTestChecker::getPtrSymbol(const Expr *E, CheckerContext &C) {
  if (!E)
    return nullptr;

  const Expr *Node = E->IgnoreParens();
  // Unwrap value-preserving implicit casts (e.g. BitCast, PointerToBoolean)
  // but stop at LValueToRValue: that node evaluates in the environment to the
  // loaded pointer value rather than to the address of the glvalue.
  while (const auto *ICE = dyn_cast<ImplicitCastExpr>(Node)) {
    if (ICE->getCastKind() == CK_LValueToRValue)
      break;
    if (!ICE->getSubExpr()->getType()->isPointerType())
      break;
    Node = ICE->getSubExpr();
  }
  if (!Node->getType()->isPointerType())
    return nullptr;

  SVal V = C.getSVal(Node);
  if (SymbolRef Sym = symbolFromSVal(V))
    return Sym;

  // Bare glvalue: load the stored pointer value from its location.
  if (auto MRV = V.getAs<loc::MemRegionVal>())
    return symbolFromSVal(
        C.getState()->getSVal(MRV->castAs<Loc>(), Node->getType()));

  return nullptr;
}

bool SAGenTestChecker::exprIsNull(const Expr *E, CheckerContext &C) {
  if (!E)
    return false;

  // Check for null pointer constant
  if (E->isNullPointerConstant(C.getASTContext(), Expr::NPC_ValueDependentIsNull))
    return true;

  // Evaluate as int constant 0
  llvm::APSInt Val;
  if (EvaluateExprToInt(Val, E, C)) {
    if (Val == 0)
      return true;
  }

  // Check textual "NULL"
  if (ExprHasName(E, "NULL", C))
    return true;

  return false;
}

SymbolRef SAGenTestChecker::extractPtrSymbolFromCond(const Expr *CondE,
                                                     CheckerContext &C) {
  if (!CondE)
    return nullptr;

  const Expr *E = CondE->IgnoreParens();

  // Handle IS_ERR_OR_NULL(ptr) style guards. A plain IS_ERR(ptr) does not
  // prove the pointer non-NULL, so it must not clear the unchecked state.
  if (const auto *CE = dyn_cast<CallExpr>(E)) {
    if (ExprHasName(CE, "IS_ERR_OR_NULL", C) && CE->getNumArgs() >= 1)
      return getPtrSymbol(CE->getArg(0), C);
    return nullptr;
  }

  // Handle if (!p)
  if (const auto *UO = dyn_cast<UnaryOperator>(E)) {
    if (UO->getOpcode() == UO_LNot)
      return getPtrSymbol(UO->getSubExpr(), C);
    return nullptr;
  }

  // Handle if (p == NULL) or if (p != NULL)
  if (const auto *BO = dyn_cast<BinaryOperator>(E)) {
    if (BO->getOpcode() == BO_EQ || BO->getOpcode() == BO_NE) {
      const Expr *LHS = BO->getLHS()->IgnoreParenImpCasts();
      const Expr *RHS = BO->getRHS()->IgnoreParenImpCasts();

      bool LHSNull = exprIsNull(LHS, C);
      bool RHSNull = exprIsNull(RHS, C);

      if (LHSNull && !RHSNull)
        return getPtrSymbol(BO->getRHS(), C);
      if (RHSNull && !LHSNull)
        return getPtrSymbol(BO->getLHS(), C);
    }
    return nullptr;
  }

  // Handle if (p): pointer-valued condition operand.
  return getPtrSymbol(E, C);
}

void SAGenTestChecker::reportPossibleNullDeref(const Stmt *S, CheckerContext &C,
                                               StringRef Extra) const {
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  llvm::SmallString<128> Msg("Possible NULL dereference of *_optional() result");
  if (!Extra.empty()) {
    Msg += " ";
    Msg += Extra;
  }

  auto R = std::make_unique<PathSensitiveBugReport>(*BT, Msg, N);
  if (S)
    R->addRange(S->getSourceRange());
  C.emitReport(std::move(R));
}

// Callback implementations

void SAGenTestChecker::checkPostCall(const CallEvent &Call, CheckerContext &C) const {
  // *_optional() getters may legitimately return NULL; record the symbolic
  // value of the returned pointer as "not NULL-checked yet".
  if (!isOptionalGetter(Call, C))
    return;

  SymbolRef Sym = symbolFromSVal(Call.getReturnValue());
  if (!Sym)
    return;

  ProgramStateRef State = C.getState();
  State = State->set<OptionalPtrMap>(Sym, 0u);
  C.addTransition(State);
}

void SAGenTestChecker::checkPreCall(const CallEvent &Call, CheckerContext &C) const {
  llvm::SmallVector<unsigned, 4> DerefParams;
  if (!functionKnownToDeref(Call, DerefParams))
    return;

  ProgramStateRef State = C.getState();
  for (unsigned Idx : DerefParams) {
    if (Idx >= Call.getNumArgs())
      continue;

    const Expr *ArgE = Call.getArgExpr(Idx);
    SymbolRef Sym = getPtrSymbol(ArgE, C);
    if (!Sym)
      continue;

    const unsigned *Flag = State->get<OptionalPtrMap>(Sym);
    if (Flag && *Flag == 0) {
      // Report: passing possibly NULL optional result to a function that dereferences it.
      llvm::SmallString<32> Extra;
      Extra += "(argument ";
      Extra += llvm::Twine(Idx).str();
      Extra += ")";
      reportPossibleNullDeref(Call.getOriginExpr(), C, Extra);
      // Do not early-return; report for all deref args.
    }
  }
}

void SAGenTestChecker::checkBranchCondition(const Stmt *Condition, CheckerContext &C) const {
  const Expr *CondE = dyn_cast_or_null<Expr>(Condition);
  if (!CondE)
    return;

  const MemRegion *MR = nullptr;
  if (!extractPtrRegionFromCond(CondE, MR, C) || !MR)
    return;

  MR = MR->getBaseRegion();
  ProgramStateRef State = C.getState();
  if (const unsigned *Flag = State->get<OptionalPtrMap>(MR)) {
    if (*Flag == 0) {
      State = setChecked(State, MR);
      C.addTransition(State);
    }
  }
}

void SAGenTestChecker::checkLocation(SVal Loc, bool IsLoad, const Stmt *S, CheckerContext &C) const {
  const MemRegion *MR = nullptr;
  if (isDerefOfTrackedRegion(S, MR, C)) {
    reportPossibleNullDeref(S, C);
  }
}

void SAGenTestChecker::checkBind(SVal Loc, SVal Val, const Stmt *S, CheckerContext &C) const {
  ProgramStateRef State = C.getState();

  const MemRegion *Dst = Loc.getAsRegion();
  const MemRegion *Src = Val.getAsRegion();
  if (!Dst || !Src) {
    // Nothing to alias-propagate
    return;
  }

  Dst = Dst->getBaseRegion();
  Src = Src->getBaseRegion();
  if (!Dst || !Src)
    return;

  // Only propagate for pointer-typed destinations
  if (const auto *TVR = dyn_cast<TypedValueRegion>(Dst)) {
    QualType Ty = TVR->getValueType();
    if (!Ty.isNull() && Ty->isPointerType()) {
      State = propagateAlias(State, Dst, Src);
      C.addTransition(State);
    }
  }
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects dereferencing results of *_optional() getters without NULL check",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
