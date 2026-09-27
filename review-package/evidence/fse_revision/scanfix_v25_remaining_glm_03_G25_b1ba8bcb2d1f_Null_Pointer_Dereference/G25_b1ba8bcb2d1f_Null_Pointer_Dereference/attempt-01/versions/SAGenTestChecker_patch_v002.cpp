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

// Program state map:
// - OptionalPtrMap: tracks the pointer VALUES (symbols) returned from
//   *_optional() getters, which may legally return NULL instead of an
//   ERR_PTR-encoded error.
//   Value: 0 = not checked for NULL yet; 1 = NULL-checked.
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
   static SymbolRef getPtrSymbolFromExpr(const Expr *E, CheckerContext &C);

   static ProgramStateRef setChecked(ProgramStateRef State, SymbolRef Sym);

   static bool exprIsNull(const Expr *E, CheckerContext &C);
   static bool extractPtrSymbolFromCond(const Expr *CondE,
                                        SymbolRef &OutSym,
                                        CheckerContext &C);

   bool isDerefOfTrackedSymbol(const Stmt *S,
                               SymbolRef &OutSym,
                               CheckerContext &C) const;

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

SymbolRef SAGenTestChecker::getPtrSymbolFromExpr(const Expr *E, CheckerContext &C) {
  if (!E)
    return nullptr;

  // Peel outer parentheses only: implicit casts are kept so that the
  // lookup reaches the environment binding of the loaded rvalue. The
  // resulting symbol identifies the pointer VALUE, which keeps its
  // identity when the value is copied into locals or struct fields
  // (e.g. lcd->im_pins = devm_gpiod_get_array_optional(...)).
  E = E->IgnoreParens();

  SVal V = C.getState()->getSVal(E, C.getLocationContext());
  return V.getAsLocSymbol();
}

ProgramStateRef SAGenTestChecker::setChecked(ProgramStateRef State, SymbolRef Sym) {
  if (!State || !Sym)
    return State;

  const unsigned *Flag = State->get<OptionalPtrMap>(Sym);
  if (Flag && *Flag == 0)
    State = State->set<OptionalPtrMap>(Sym, 1);

  return State;
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

bool SAGenTestChecker::extractPtrSymbolFromCond(const Expr *CondE,
                                                SymbolRef &OutSym,
                                                CheckerContext &C) {
  OutSym = nullptr;
  if (!CondE)
    return false;

  const Expr *E = CondE->IgnoreParenImpCasts();

  // Handle IS_ERR_OR_NULL(ptr) style. Note that plain IS_ERR(p) does not
  // rule out NULL for an optional getter, so it must never mark the
  // tracked value as checked.
  if (const auto *CE = dyn_cast<CallExpr>(E)) {
    if (ExprHasName(CE, "IS_ERR_OR_NULL", C) && CE->getNumArgs() >= 1) {
      SymbolRef Sym = getPtrSymbolFromExpr(CE->getArg(0), C);
      if (Sym) {
        OutSym = Sym;
        return true;
      }
    }
  }

  // Handle if (!p)
  if (const auto *UO = dyn_cast<UnaryOperator>(E)) {
    if (UO->getOpcode() == UO_LNot) {
      SymbolRef Sym = getPtrSymbolFromExpr(UO->getSubExpr(), C);
      if (Sym) {
        OutSym = Sym;
        return true;
      }
    }
  }

  // Handle if (p == NULL) or if (p != NULL)
  if (const auto *BO = dyn_cast<BinaryOperator>(E)) {
    if (BO->getOpcode() == BO_EQ || BO->getOpcode() == BO_NE) {
      const Expr *LHS = BO->getLHS()->IgnoreParenImpCasts();
      const Expr *RHS = BO->getRHS()->IgnoreParenImpCasts();

      bool LHSNull = exprIsNull(LHS, C);
      bool RHSNull = exprIsNull(RHS, C);

      if (LHSNull && !RHSNull) {
        SymbolRef Sym = getPtrSymbolFromExpr(BO->getRHS(), C);
        if (Sym) {
          OutSym = Sym;
          return true;
        }
      } else if (RHSNull && !LHSNull) {
        SymbolRef Sym = getPtrSymbolFromExpr(BO->getLHS(), C);
        if (Sym) {
          OutSym = Sym;
          return true;
        }
      }
    }
  }

  // Handle if (p)
  // Only if the expression's type is pointer and we can get its value
  // symbol; use the original condition so implicit casts are preserved
  // and the environment binding of the loaded value is found.
  if (E->getType()->isPointerType()) {
    SymbolRef Sym = getPtrSymbolFromExpr(CondE, C);
    if (Sym) {
      OutSym = Sym;
      return true;
    }
  }

  return false;
}

bool SAGenTestChecker::isDerefOfTrackedSymbol(const Stmt *S,
                                              SymbolRef &OutSym,
                                              CheckerContext &C) const {
  OutSym = nullptr;
  if (!S)
    return false;

  const ProgramStateRef State = C.getState();

  // Member access via '->'
  if (const auto *ME = dyn_cast<MemberExpr>(S)) {
    if (ME->isArrow()) {
      SymbolRef Sym = getPtrSymbolFromExpr(ME->getBase(), C);
      if (Sym) {
        const unsigned *Flag = State->get<OptionalPtrMap>(Sym);
        if (Flag && *Flag == 0) { // not null-checked
          OutSym = Sym;
          return true;
        }
      }
    }
  }

  // Array access: p[i]
  if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(S)) {
    SymbolRef Sym = getPtrSymbolFromExpr(ASE->getBase(), C);
    if (Sym) {
      const unsigned *Flag = State->get<OptionalPtrMap>(Sym);
      if (Flag && *Flag == 0) {
        OutSym = Sym;
        return true;
      }
    }
  }

  // Unary dereference: *p
  if (const auto *UO = dyn_cast<UnaryOperator>(S)) {
    if (UO->getOpcode() == UO_Deref) {
      SymbolRef Sym = getPtrSymbolFromExpr(UO->getSubExpr(), C);
      if (Sym) {
        const unsigned *Flag = State->get<OptionalPtrMap>(Sym);
        if (Flag && *Flag == 0) {
          OutSym = Sym;
          return true;
        }
      }
    }
  }

  return false;
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
  ProgramStateRef State = C.getState();

  // Track returns from *_optional() getters. Such getters may legally
  // return NULL; track the returned pointer VALUE (symbol), which keeps
  // its identity when stored into struct fields or locals.
  if (isOptionalGetter(Call, C)) {
    SymbolRef RetSym = Call.getReturnValue().getAsLocSymbol();
    if (RetSym) {
      // Insert as "not checked yet" (0)
      State = State->set<OptionalPtrMap>(RetSym, 0u);
      C.addTransition(State);
    }
  }
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
    SymbolRef Sym = getPtrSymbolFromExpr(ArgE, C);
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

  SymbolRef Sym = nullptr;
  if (!extractPtrSymbolFromCond(CondE, Sym, C) || !Sym)
    return;

  ProgramStateRef State = C.getState();
  if (const unsigned *Flag = State->get<OptionalPtrMap>(Sym)) {
    if (*Flag == 0) {
      State = setChecked(State, Sym);
      C.addTransition(State);
    }
  }
}

void SAGenTestChecker::checkLocation(SVal Loc, bool IsLoad, const Stmt *S, CheckerContext &C) const {
  (void)Loc;
  (void)IsLoad;
  SymbolRef Sym = nullptr;
  if (isDerefOfTrackedSymbol(S, Sym, C)) {
    reportPossibleNullDeref(S, C);
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
