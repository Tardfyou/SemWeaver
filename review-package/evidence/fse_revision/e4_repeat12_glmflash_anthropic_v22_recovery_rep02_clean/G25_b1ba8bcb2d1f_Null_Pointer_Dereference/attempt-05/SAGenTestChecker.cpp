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

// Program state maps:
// - OptionalPtrMap: tracks pointers returned from *_optional() getters
//   Value: 0 = not checked for NULL yet; 1 = NULL-checked
REGISTER_MAP_WITH_PROGRAMSTATE(OptionalPtrMap, const MemRegion*, unsigned)
// - PtrAliasMap: simple bidirectional alias map between pointer regions
REGISTER_MAP_WITH_PROGRAMSTATE(PtrAliasMap, const MemRegion*, const MemRegion*)

namespace {

class SAGenTestChecker
  : public Checker<
        check::PostCall,
        check::PreCall,
        check::BranchCondition,
        check::Location,
        check::Bind> {

   mutable std::unique_ptr<BugType> BT;

public:
   SAGenTestChecker()
     : BT(new BugType(this, "NULL dereference of *_optional() result", "API Misuse")) {}

   void checkPostCall(const CallEvent &Call, CheckerContext &C) const;
   void checkPreCall(const CallEvent &Call, CheckerContext &C) const;
   void checkBranchCondition(const Stmt *Condition, CheckerContext &C) const;
   void checkLocation(SVal Loc, bool IsLoad, const Stmt *S, CheckerContext &C) const;
   void checkBind(SVal Loc, SVal Val, const Stmt *S, CheckerContext &C) const;

private:
   // Helpers
   static bool isOptionalGetter(const CallEvent &Call, CheckerContext &C);
   static const MemRegion* getRegionFromExprLike(const Expr *E, CheckerContext &C);
   static const MemRegion *resolveDerefKey(const Expr *Base,
                                           const ProgramStateRef &State,
                                           CheckerContext &C);

   static ProgramStateRef propagateAlias(ProgramStateRef State,
                                         const MemRegion *Dst,
                                         const MemRegion *Src);
   static ProgramStateRef setChecked(ProgramStateRef State, const MemRegion *R);

   static bool exprIsNull(const Expr *E, CheckerContext &C);
   static bool extractPtrRegionFromCond(const Expr *CondE,
                                        const MemRegion *&OutR,
                                        CheckerContext &C);

   bool isDerefOfTrackedRegion(const Stmt *S,
                               const MemRegion *&OutR,
                               CheckerContext &C) const;

   const Expr *findUncheckedDerefIn(const Stmt *S, CheckerContext &C) const;

   void reportPossibleNullDeref(const Stmt *S, CheckerContext &C,
                                StringRef Extra = "") const;
};

bool SAGenTestChecker::isOptionalGetter(const CallEvent &Call, CheckerContext &C) {
  const Expr *Origin = Call.getOriginExpr();
  if (!Origin)
    return false;

  // Kernel *_optional() getter contract: the callee's declared name carries the
  // "_optional" marker family and the returned pointer may legitimately be NULL
  // when the requested resource is absent. Resolve the name from the callee
  // declaration so recognition does not depend on how the call is spelled at
  // the call site; keep the source text only as a fallback.
  bool IsOptionalName = false;
  if (const auto *CE = dyn_cast<CallExpr>(Origin))
    if (const FunctionDecl *FD = CE->getDirectCallee())
      IsOptionalName = FD->getName().contains("_optional");
  if (!IsOptionalName && !ExprHasName(Origin, "_optional", C))
    return false;

  // Ensure it returns a pointer type
  QualType RTy = Call.getResultType();
  if (RTy.isNull() || !RTy->isPointerType())
    return false;

  return true;
}

const MemRegion* SAGenTestChecker::getRegionFromExprLike(const Expr *E, CheckerContext &C) {
  if (!E)
    return nullptr;

  const Expr *Stripped = E->IgnoreParenImpCasts();

  // Canonicalize pointer-typed lvalue accesses to their storage regions so
  // the producing call, the storing bind, the null-guard branch, and the
  // later dereference all resolve the same key even when the carried value
  // is an opaque symbol:
  // - a plain variable: its VarRegion
  if (const auto *DRE = dyn_cast<DeclRefExpr>(Stripped)) {
    if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
      if (const MemRegion *R = C.getState()->getLValue(VD, C.getLocationContext()).getAsRegion())
        return R;
    }
  }
  // - a field access (a.f or a->f): the FieldRegion of the accessed object.
  //   The super region comes from the base's storage region; when the base
  //   value is an opaque symbol (for example a pointer from an unmodeled
  //   allocator), build the symbolic super region so the key stays canonical.
  if (const auto *ME = dyn_cast<MemberExpr>(Stripped)) {
    if (const auto *FD = dyn_cast<FieldDecl>(ME->getMemberDecl())) {
      const MemRegion *Super = getMemRegionFromExpr(ME->getBase(), C);
      if (!Super) {
        if (SymbolRef Sym = C.getState()->getSVal(ME->getBase(), C.getLocationContext()).getAsSymbol())
          Super = C.getSValBuilder().getRegionManager().getSymbolicRegion(Sym);
      }
      // getFieldRegion requires a SubRegion super region; VarRegion and
      // SymbolicRegion both satisfy this. Fall through otherwise.
      if (const auto *SuperSR = dyn_cast_or_null<SubRegion>(Super))
        return C.getSValBuilder().getRegionManager().getFieldRegion(FD, SuperSR);
    }
  }

  // First, try directly.
  if (const MemRegion *MR = getMemRegionFromExpr(E, C)) {
    return MR ? MR->getBaseRegion() : nullptr;
  }

  // A pointer value can also be carried as a plain symbol (e.g. a conjured
  // *_optional() call result loaded back from storage). Normalize it to its
  // canonical symbolic region so the producing call, the storing bind, the
  // null-guard branch, and later dereferences all resolve to the same key.
  if (SymbolRef Sym = C.getState()->getSVal(E, C.getLocationContext()).getAsSymbol()) {
    return C.getSValBuilder().getRegionManager().getSymbolicRegion(Sym)->getBaseRegion();
  }

  // Try peeling simple wrappers and querying again.
  if (const auto *ICE = dyn_cast<ImplicitCastExpr>(E)) {
    if (const MemRegion *MR = getMemRegionFromExpr(ICE->getSubExpr(), C))
      return MR->getBaseRegion();
  }
  if (const auto *PE = dyn_cast<ParenExpr>(E)) {
    if (const MemRegion *MR = getMemRegionFromExpr(PE->getSubExpr(), C))
      return MR->getBaseRegion();
  }

  // Try to find a DeclRefExpr or MemberExpr child
  if (const auto *DRE = findSpecificTypeInChildren<DeclRefExpr>(E)) {
    if (const MemRegion *MR = getMemRegionFromExpr(DRE, C))
      return MR->getBaseRegion();
  }
  if (const auto *ME = findSpecificTypeInChildren<MemberExpr>(E)) {
    if (const MemRegion *MR = getMemRegionFromExpr(ME, C))
      return MR->getBaseRegion();
  }

  return nullptr;
}

// The tracking key for a dereferenced base expression. Prefer the base's
// storage region; when that region is not itself tracked, fall back to the
// region of the loaded base value: the *_optional() result is keyed by the
// produced value's symbol at the producing call, and that same value is
// loaded back unchanged at a null guard and at a dereference site. This value
// relation ties the producing call, the guard, and the sink together even
// when storage-region resolution differs between the sites.
const MemRegion *SAGenTestChecker::resolveDerefKey(const Expr *Base,
                                                   const ProgramStateRef &State,
                                                   CheckerContext &C) {
  const MemRegion *MR = Base ? getRegionFromExprLike(Base, C) : nullptr;
  if (MR && State->get<OptionalPtrMap>(MR))
    return MR;

  if (Base) {
    if (SymbolRef Sym = State->getSVal(Base, C.getLocationContext()).getAsSymbol()) {
      const MemRegion *SymR =
          C.getSValBuilder().getRegionManager().getSymbolicRegion(Sym);
      if (State->get<OptionalPtrMap>(SymR))
        return SymR;
    }
  }

  return MR;
}

ProgramStateRef SAGenTestChecker::propagateAlias(ProgramStateRef State,
                                                 const MemRegion *Dst,
                                                 const MemRegion *Src) {
  if (!State || !Dst || !Src)
    return State;

  // If Src is tracked as optional, propagate the same flag to Dst.
  if (const unsigned *Flag = State->get<OptionalPtrMap>(Src)) {
    State = State->set<OptionalPtrMap>(Dst, *Flag);
  }

  // Record aliases in both directions (one-step)
  State = State->set<PtrAliasMap>(Dst, Src);
  State = State->set<PtrAliasMap>(Src, Dst);
  return State;
}

ProgramStateRef SAGenTestChecker::setChecked(ProgramStateRef State, const MemRegion *R) {
  if (!State || !R)
    return State;

  const unsigned *Flag = State->get<OptionalPtrMap>(R);
  if (Flag && *Flag == 0)
    State = State->set<OptionalPtrMap>(R, 1);

  // Also set checked for one-step aliases (both directions are recorded).
  if (const MemRegion *const *AliasPtr = State->get<PtrAliasMap>(R)) {
    const MemRegion *Alias = *AliasPtr;
    const unsigned *AFlag = State->get<OptionalPtrMap>(Alias);
    if (AFlag && *AFlag == 0)
      State = State->set<OptionalPtrMap>(Alias, 1);
  }

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

bool SAGenTestChecker::extractPtrRegionFromCond(const Expr *CondE,
                                                const MemRegion *&OutR,
                                                CheckerContext &C) {
  OutR = nullptr;
  if (!CondE)
    return false;

  const Expr *E = CondE->IgnoreParenImpCasts();

  // Handle IS_ERR_OR_NULL(ptr) style
  if (const auto *CE = dyn_cast<CallExpr>(E)) {
    if (ExprHasName(CE, "IS_ERR_OR_NULL", C) && CE->getNumArgs() >= 1) {
      const Expr *Arg0 = CE->getArg(0);
      const MemRegion *MR = getRegionFromExprLike(Arg0, C);
      if (MR) {
        OutR = MR;
        return true;
      }
    }
  }

  // Handle if (!p)
  if (const auto *UO = dyn_cast<UnaryOperator>(E)) {
    if (UO->getOpcode() == UO_LNot) {
      const MemRegion *MR = getRegionFromExprLike(UO->getSubExpr(), C);
      if (MR) {
        OutR = MR;
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
        const MemRegion *MR = getRegionFromExprLike(RHS, C);
        if (MR) {
          OutR = MR;
          return true;
        }
      } else if (RHSNull && !LHSNull) {
        const MemRegion *MR = getRegionFromExprLike(LHS, C);
        if (MR) {
          OutR = MR;
          return true;
        }
      }
    }
  }

  // Handle if (p)
  // Only if the expression's type is pointer and we can get a region.
  if (E->getType()->isPointerType()) {
    const MemRegion *MR = getRegionFromExprLike(E, C);
    if (MR) {
      OutR = MR;
      return true;
    }
  }

  return false;
}

bool SAGenTestChecker::isDerefOfTrackedRegion(const Stmt *S,
                                              const MemRegion *&OutR,
                                              CheckerContext &C) const {
  OutR = nullptr;
  if (!S)
    return false;

  const ProgramStateRef State = C.getState();

  // Member access via '->'
  if (const auto *ME = dyn_cast<MemberExpr>(S)) {
    if (ME->isArrow()) {
      const Expr *Base = ME->getBase();
      const MemRegion *MR = resolveDerefKey(Base, State, C);
      if (MR) {
        if (const unsigned *Flag = State->get<OptionalPtrMap>(MR)) {
          if (*Flag == 0) { // not null-checked
            OutR = MR;
            return true;
          }
        }
      }
    }
  }

  // Array access: p[i]
  if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(S)) {
    const Expr *Base = ASE->getBase();
    const MemRegion *MR = resolveDerefKey(Base, State, C);
    if (MR) {
      if (const unsigned *Flag = State->get<OptionalPtrMap>(MR)) {
        if (*Flag == 0) {
          OutR = MR;
          return true;
        }
      }
    }
  }

  // Unary dereference: *p
  if (const auto *UO = dyn_cast<UnaryOperator>(S)) {
    if (UO->getOpcode() == UO_Deref) {
      const Expr *Sub = UO->getSubExpr();
      const MemRegion *MR = resolveDerefKey(Sub, State, C);
      if (MR) {
        if (const unsigned *Flag = State->get<OptionalPtrMap>(MR)) {
          if (*Flag == 0) {
            OutR = MR;
            return true;
          }
        }
      }
    }
  }

  return false;
}

const Expr *SAGenTestChecker::findUncheckedDerefIn(const Stmt *S,
                                                   CheckerContext &C) const {
  if (!S)
    return nullptr;

  // A direct dereference of a tracked *_optional() result that has not been
  // NULL-checked yet.
  const MemRegion *R = nullptr;
  if (isDerefOfTrackedRegion(S, R, C) && R) {
    if (const auto *E = dyn_cast<Expr>(S))
      return E;
  }

  // Recurse so that a dereference nested inside a larger condition
  // expression (for example "if (p->ndescs < K)") is still found.
  for (const Stmt *Child : S->children()) {
    if (const Expr *Found = findUncheckedDerefIn(Child, C))
      return Found;
  }

  return nullptr;
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

  // Track returns from *_optional() getters.
  if (isOptionalGetter(Call, C)) {
    const MemRegion *RetR = Call.getReturnValue().getAsRegion();
    if (!RetR) {
      // A conjured pointer result can surface as a plain symbol; normalize
      // it to its symbolic region so the tracking key stays stable when the
      // result is stored into a field and later dereferenced.
      if (SymbolRef Sym = Call.getReturnValue().getAsSymbol())
        RetR = C.getSValBuilder().getRegionManager().getSymbolicRegion(Sym);
    }
    if (RetR) {
      RetR = RetR->getBaseRegion();
      // Insert as "not checked yet" (0)
      State = State->set<OptionalPtrMap>(RetR, 0u);
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
    const MemRegion *MR = getRegionFromExprLike(ArgE, C);
    if (!MR)
      continue;

    const unsigned *Flag = State->get<OptionalPtrMap>(MR);
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

  // Mark the pointer NULL-checked when this condition is itself a null
  // guard ("if (p)", "if (!p)", "if (p == NULL)"). A condition that is not
  // a null guard (for example "if (p->ndescs < K)") must not abort here:
  // the dereference it contains is evaluated before any later guard of the
  // same pointer and is still scanned below.
  const MemRegion *MR = nullptr;
  if (extractPtrRegionFromCond(CondE, MR, C) && MR) {
    ProgramStateRef State = C.getState();
    if (const unsigned *Flag = State->get<OptionalPtrMap>(MR)) {
      if (*Flag == 0) {
        State = setChecked(State, MR);
        // A null guard constrains the guarded value itself: also clear the
        // tracked *_optional() result symbol loaded by this condition, so
        // later dereference sites keyed by the produced value stay silent
        // inside the guarded region.
        if (SymbolRef Sym =
                State->getSVal(CondE, C.getLocationContext()).getAsSymbol()) {
          const MemRegion *SymR =
              C.getSValBuilder().getRegionManager().getSymbolicRegion(Sym);
          if (State->get<OptionalPtrMap>(SymR))
            State = setChecked(State, SymR);
        }
        C.addTransition(State);
      }
    }
  }

  // A dereference can be nested inside the condition itself (for example
  // "if (p->n < K)"). It is evaluated before this branch could act as the
  // pointer's null guard, so when it reads through a tracked *_optional()
  // result that has not been NULL-checked yet, report it at the dereference.
  if (const Expr *DerefE = findUncheckedDerefIn(CondE, C)) {
    reportPossibleNullDeref(DerefE, C);
    return;
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

  // Canonicalize the store destination from the assignment AST: a store into
  // a field of an object whose base value is an opaque symbol must resolve to
  // the same storage region that the later null guard and dereference use.
  const MemRegion *Dst = nullptr;
  if (const auto *BO = dyn_cast_or_null<BinaryOperator>(S)) {
    if (BO->getOpcode() == BO_Assign)
      Dst = getRegionFromExprLike(BO->getLHS(), C);
  }
  if (!Dst)
    Dst = Loc.getAsRegion();

  const MemRegion *Src = Val.getAsRegion();
  if (!Src) {
    // The stored pointer can be carried as a plain symbol (e.g. a conjured
    // *_optional() result); normalize it so alias propagation still fires.
    if (SymbolRef Sym = Val.getAsSymbol())
      Src = C.getSValBuilder().getRegionManager().getSymbolicRegion(Sym);
  }
  if (!Dst || !Src) {
    // Nothing to alias-propagate
    return;
  }

  // The destination stays the exact storage region (a VarRegion for a local
  // pointer or a FieldRegion for a field such as im_pins): that is the same
  // key the later null-guard branch and the dereference site resolve through
  // getRegionFromExprLike. Only the source value is canonicalized to its
  // base (symbolic) region.
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
