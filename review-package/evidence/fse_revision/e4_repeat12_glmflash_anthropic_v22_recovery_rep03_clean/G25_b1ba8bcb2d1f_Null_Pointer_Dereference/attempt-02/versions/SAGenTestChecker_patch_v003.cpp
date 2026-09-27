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

   void reportPossibleNullDeref(const Stmt *S, CheckerContext &C,
                                StringRef Extra = "") const;
};

bool SAGenTestChecker::isOptionalGetter(const CallEvent &Call, CheckerContext &C) {
  const Expr *Origin = Call.getOriginExpr();
  if (!Origin)
    return false;

  // The nullable-result contract of this getter family is carried by the
  // callee identifier itself. Accept the resolved callee identifier (or
  // declaration name) as well as the source spelling, so tracking does
  // not depend on raw source-text extraction alone.
  bool NameMatches = false;
  if (const IdentifierInfo *II = Call.getCalleeIdentifier())
    NameMatches = II->getName().contains("_optional");
  if (!NameMatches) {
    if (const auto *FD = dyn_cast_or_null<FunctionDecl>(Call.getDecl()))
      NameMatches = StringRef(FD->getNameAsString()).contains("_optional");
  }
  if (!NameMatches && !ExprHasName(Origin, "_optional", C))
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

  // First, try directly.
  if (const MemRegion *MR = getMemRegionFromExpr(E, C)) {
    return MR ? MR->getBaseRegion() : nullptr;
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

ProgramStateRef SAGenTestChecker::propagateAlias(ProgramStateRef State,
                                                 const MemRegion *Dst,
                                                 const MemRegion *Src) {
  if (!State || !Dst || !Src)
    return State;

  // The suspect property belongs to the stored pointer value. Key the
  // state on the exact destination region (e.g. the struct field that
  // holds the optional pointer), not on the enclosing object region, so
  // ordinary member reads of the object are not blamed. Record the alias
  // in both directions so a guard on either spelling clears the state.
  if (const unsigned *Flag = State->get<OptionalPtrMap>(Src)) {
    State = State->set<OptionalPtrMap>(Dst, *Flag);
    State = State->set<PtrAliasMap>(Dst, Src);
    State = State->set<PtrAliasMap>(Src, Dst);
  }
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

  // A region is a suspect when the program state still marks it as an
  // unchecked *_optional() result value.
  auto uncheckedOn = [&](const MemRegion *R) -> const MemRegion * {
    if (!R)
      return nullptr;
    R = R->getBaseRegion();
    const unsigned *Flag = State->get<OptionalPtrMap>(R);
    if (Flag && *Flag == 0)
      return R;
    return nullptr;
  };

  // Storage-keyed lookup: a dereference base may be spelled as a member
  // read whose storage region itself was assigned the tracked maybe-NULL
  // value (the value the patch later guards with a NULL test). Keying on
  // that storage region in the state keeps the trigger alive even when
  // the getter's return symbol no longer reaches the load, and it stays
  // silent once a real NULL guard has marked the value checked (the
  // guard clears both alias spellings).
  auto uncheckedMemberStorage = [&](const Expr *BaseE) -> const MemRegion * {
    const Expr *B = BaseE ? BaseE->IgnoreParenImpCasts() : nullptr;
    const auto *BME = dyn_cast_or_null<MemberExpr>(B);
    if (!BME)
      return nullptr;
    const auto *FD = dyn_cast_or_null<FieldDecl>(BME->getMemberDecl());
    if (!FD)
      return nullptr;
    const MemRegion *ObjR = getRegionFromExprLike(BME->getBase(), C);
    const auto *ObjSR = dyn_cast_or_null<SubRegion>(ObjR);
    if (!ObjSR)
      return nullptr;
    const MemRegion *FieldR =
        C.getSValBuilder().getRegionManager().getFieldRegion(FD, ObjSR);
    const unsigned *Flag = State->get<OptionalPtrMap>(FieldR);
    if (Flag && *Flag == 0)
      return FieldR;
    return nullptr;
  };

  // Member access via '->'
  if (const auto *ME = dyn_cast<MemberExpr>(S)) {
    if (ME->isArrow()) {
      const Expr *Base = ME->getBase();
      const MemRegion *Hit = uncheckedOn(getRegionFromExprLike(Base, C));
      if (!Hit)
        Hit = uncheckedMemberStorage(Base);
      if (Hit) {
        OutR = Hit;
        return true;
      }
    }
  }

  // Array access: p[i]
  if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(S)) {
    const Expr *Base = ASE->getBase();
    const MemRegion *Hit = uncheckedOn(getRegionFromExprLike(Base, C));
    if (!Hit)
      Hit = uncheckedMemberStorage(Base);
    if (Hit) {
      OutR = Hit;
      return true;
    }
  }

  // Unary dereference: *p
  if (const auto *UO = dyn_cast<UnaryOperator>(S)) {
    if (UO->getOpcode() == UO_Deref) {
      const Expr *Sub = UO->getSubExpr();
      const MemRegion *Hit = uncheckedOn(getRegionFromExprLike(Sub, C));
      if (!Hit)
        Hit = uncheckedMemberStorage(Sub);
      if (Hit) {
        OutR = Hit;
        return true;
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

  // Track returns from *_optional() getters.
  if (isOptionalGetter(Call, C)) {
    const MemRegion *RetR = Call.getReturnValue().getAsRegion();
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
    MR = MR->getBaseRegion();

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
  const ProgramStateRef State = C.getState();

  const MemRegion *Suspect = nullptr;

  // Primary sink signal: the region actually being accessed. For `p->f`
  // or `p[i]` it is a subregion (field/element) of the pointee of the
  // tracked maybe-NULL optional pointer. Requiring a subregion keeps
  // plain reads of the pointer itself out of the sink.
  if (const MemRegion *LR = Loc.getAsRegion()) {
    const MemRegion *Base = LR->getBaseRegion();
    if (Base && Base != LR) {
      const unsigned *Flag = State->get<OptionalPtrMap>(Base);
      if (Flag && *Flag == 0)
        Suspect = Base;
    }
  }

  // Fallback: syntactic dereference forms (*p, p->f, p[i]).
  if (!Suspect) {
    const MemRegion *MR = nullptr;
    if (isDerefOfTrackedRegion(S, MR, C))
      Suspect = MR;
  }

  if (!Suspect)
    return;

  // Stay silent when this path already proves the tracked symbolic
  // pointer non-null, e.g. after an explicit guard on the way here.
  if (const auto *SR = dyn_cast<SymbolicRegion>(Suspect)) {
    ProgramStateRef NullPath =
        State->assume(C.getSValBuilder().makeLoc(SR), false);
    if (!NullPath)
      return;
  }

  reportPossibleNullDeref(S, C);
}

void SAGenTestChecker::checkBind(SVal Loc, SVal Val, const Stmt *S, CheckerContext &C) const {
  ProgramStateRef State = C.getState();

  const MemRegion *Dst = Loc.getAsRegion();
  const MemRegion *Src = Val.getAsRegion();
  if (!Dst || !Src) {
    // Nothing to alias-propagate
    return;
  }

  // Keep the exact destination region: for `obj->field = p` the suspect
  // pointer value is held by the field region itself; stripping it to the
  // base object region would make every member access of that object look
  // like a dereference of the optional pointer.
  Src = Src->getBaseRegion();
  if (!Src)
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
