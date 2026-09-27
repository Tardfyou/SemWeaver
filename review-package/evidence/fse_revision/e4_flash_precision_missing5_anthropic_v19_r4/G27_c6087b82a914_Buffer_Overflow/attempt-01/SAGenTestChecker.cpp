#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Checkers/Taint.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/Environment.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramState.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramStateTrait.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SValBuilder.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SymExpr.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtVisitor.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/StaticAnalyzer/Checkers/utility.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Decl.h"

using namespace clang;
using namespace ento;
using namespace taint;

// Program state map: pointer MemRegion -> destination fixed-size array decl
REGISTER_MAP_WITH_PROGRAMSTATE(BufAliasMap, const MemRegion *, const ValueDecl *)

// Length variable -> fixed-size array whose sizeof() bounds it through an
// explicit clamp assignment: len = min(count, sizeof(arr) - 1)
REGISTER_MAP_WITH_PROGRAMSTATE(LenBoundMap, const VarDecl *, const ValueDecl *)

namespace {

// Capacity of the fixed-size array declared by D, if any.
static bool getConstantArrayCap(const ValueDecl *D, llvm::APInt &ArrSize) {
  if (!D)
    return false;
  const auto *CAT = dyn_cast<ConstantArrayType>(D->getType().getTypePtr());
  if (!CAT)
    return false;
  ArrSize = CAT->getSize();
  return true;
}

// Recover the declaration carrying a fixed-size array capacity from a memory
// region: strip casts first, then trace back through element regions to the
// VarRegion/FieldRegion whose declaration has a constant array type. This
// keeps decayed destinations (&mybuf[0], arr[i], record->arrfield) resolvable
// instead of silently dropping them.
static const ValueDecl *getFixedArrayDeclFromRegion(const MemRegion *R,
                                                    llvm::APInt &ArrSize) {
  if (!R)
    return nullptr;
  R = R->StripCasts();
  while (R) {
    if (const auto *ER = dyn_cast<ElementRegion>(R)) {
      R = ER->getSuperRegion();
      if (R)
        R = R->StripCasts();
      continue;
    }
    const ValueDecl *D = nullptr;
    if (const auto *VR = dyn_cast<VarRegion>(R))
      D = VR->getDecl();
    else if (const auto *FR = dyn_cast<FieldRegion>(R))
      D = FR->getDecl();
    else
      return nullptr;
    if (getConstantArrayCap(D, ArrSize))
      return D;
    return nullptr;
  }
  return nullptr;
}

// Find a fixed-size array whose sizeof() appears inside S and report the
// declaration and capacity of that array.
static bool findSizeofFixedArray(const Stmt *S, const ValueDecl *&ArrDecl,
                                 llvm::APInt &ArrSize) {
  if (!S)
    return false;
  if (const auto *SE = dyn_cast<UnaryExprOrTypeTraitExpr>(S)) {
    if (SE->getKind() != UETT_SizeOf || SE->isArgumentType())
      return false;
    if (const auto *DRE =
            dyn_cast<DeclRefExpr>(SE->getArgumentExpr()->IgnoreParenImpCasts())) {
      if (getConstantArrayCap(DRE->getDecl(), ArrSize)) {
        ArrDecl = DRE->getDecl();
        return true;
      }
    }
    return false;
  }
  for (const Stmt *Child : S->children())
    if (findSizeofFixedArray(Child, ArrDecl, ArrSize))
      return true;
  return false;
}

// Bound relation of an explicit clamp assignment such as
// len = min(count, sizeof(arr) - 1): the kernel min()/clamp() macros expand
// to select-shaped expressions (GNU statement expressions,
// __builtin_choose_expr, comparison conditionals), while other code bases
// call min()/clamp() directly.
static bool getMinClampBound(const Expr *RHS, CheckerContext &C,
                             const ValueDecl *&Bound, llvm::APInt &BoundCap) {
  if (!RHS)
    return false;
  RHS = RHS->IgnoreParenImpCasts();

  if (const auto *CE = dyn_cast<CallExpr>(RHS)) {
    if (!(ExprHasName(CE->getCallee(), "min", C) ||
          ExprHasName(CE->getCallee(), "clamp", C)))
      return false;
    for (const Expr *Arg : CE->arguments())
      if (findSizeofFixedArray(Arg, Bound, BoundCap))
        return true;
    return false;
  }

  if (const auto *SExpr = dyn_cast<StmtExpr>(RHS))
    return findSizeofFixedArray(SExpr, Bound, BoundCap);
  if (const auto *CH = dyn_cast<ChooseExpr>(RHS))
    return findSizeofFixedArray(CH, Bound, BoundCap);
  if (const auto *CO = dyn_cast<ConditionalOperator>(RHS)) {
    const auto *CondBO =
        dyn_cast<BinaryOperator>(CO->getCond()->IgnoreParenImpCasts());
    if (!CondBO || !CondBO->isComparisonOp())
      return false;
    return findSizeofFixedArray(CO->getLHS(), Bound, BoundCap) ||
           findSizeofFixedArray(CO->getRHS(), Bound, BoundCap);
  }
  return false;
}

class SAGenTestChecker : public Checker<check::PreCall, check::Bind> {
   mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "copy_from_user into fixed-size buffer",
                       "Memory Error")) {}

  void checkPreCall(const CallEvent &Call, CheckerContext &C) const;
  void checkBind(SVal Loc, SVal Val, const Stmt *S, CheckerContext &C) const;

private:
  // Helpers
  bool isCopyFromUser(const CallEvent &Call, CheckerContext &C) const;
  bool getArrayFromDestExpr(const Expr *Dest, const ValueDecl *&ArrDecl,
                            llvm::APInt &ArrSize,
                            CheckerContext &C) const;
  bool lenExprLooksUnboundedUserCount(const Expr *LenE,
                                      const ValueDecl *ArrDecl,
                                      CheckerContext &C) const;
  bool lenIsProvablyBounded(const Expr *LenE, const llvm::APInt &ArrSize,
                            CheckerContext &C) const;
  void reportUnbounded(const CallEvent &Call, const Expr *DestE,
                       CheckerContext &C, StringRef Msg) const;
};

bool SAGenTestChecker::isCopyFromUser(const CallEvent &Call,
                                      CheckerContext &C) const {
  const Expr *Origin = Call.getOriginExpr();
  if (!Origin)
    return false;
  // Use source-based name matching for robustness as suggested.
  if (ExprHasName(Origin, "copy_from_user", C))
    return true;
  if (ExprHasName(Origin, "__copy_from_user", C))
    return true;
  return false;
}

bool SAGenTestChecker::getArrayFromDestExpr(const Expr *Dest,
                                            const ValueDecl *&ArrDecl,
                                            llvm::APInt &ArrSize,
                                            CheckerContext &C) const {
  ArrDecl = nullptr;

  // Try direct array use: e.g., copy_from_user(mybuf, ...)
  if (const auto *DRE =
          dyn_cast<DeclRefExpr>(Dest->IgnoreParenImpCasts())) {
    if (getConstantArrayCap(DRE->getDecl(), ArrSize)) {
      ArrDecl = DRE->getDecl();
      return true;
    }
  }

  // Recover the capacity from the destination region: strip casts and walk
  // element/super regions so decayed destinations such as &mybuf[0], arr[i]
  // or record->arrfield resolve to the declaration with the constant size.
  const MemRegion *MR = getMemRegionFromExpr(Dest, C);
  if (MR) {
    if (const ValueDecl *D = getFixedArrayDeclFromRegion(MR, ArrSize)) {
      ArrDecl = D;
      return true;
    }

    // Try alias via program state map: e.g., pbuf = mybuf; copy_from_user(pbuf, ...)
    MR = MR->getBaseRegion();
    if (MR) {
      ProgramStateRef State = C.getState();
      const ValueDecl *const *VDPtr = State->get<BufAliasMap>(MR);
      if (VDPtr && *VDPtr && getConstantArrayCap(*VDPtr, ArrSize)) {
        ArrDecl = *VDPtr;
        return true;
      }
    }
  }

  return false;
}

bool SAGenTestChecker::lenExprLooksUnboundedUserCount(const Expr *LenE,
                                                      const ValueDecl *ArrDecl,
                                                      CheckerContext &C) const {
  if (!LenE)
    return false;

  // Suppress if there is a clear clamp via min(...)/clamp(...)
  if (ExprHasName(LenE, "min", C) || ExprHasName(LenE, "clamp", C))
    return false;

  // Suppress if expression mentions sizeof(array)
  if (ArrDecl) {
    if (ExprHasName(LenE, "sizeof", C) &&
        ExprHasName(LenE, ArrDecl->getName(), C))
      return false;
  }

  // The length is directly the user-count parameter of the enclosing
  // handler: attacker controlled and, absent a proven bound on this path,
  // unbounded for the destination.
  const auto *DRE = dyn_cast<DeclRefExpr>(LenE->IgnoreParenImpCasts());
  if (!DRE)
    return false;
  const auto *PVD = dyn_cast<ParmVarDecl>(DRE->getDecl());
  if (!PVD)
    return false;

  const LocationContext *LC = C.getLocationContext();
  if (!LC)
    return false;
  const auto *FD = dyn_cast_or_null<FunctionDecl>(LC->getDecl());
  if (!FD)
    return false;
  for (unsigned I = 0; I < FD->getNumParams(); ++I)
    if (FD->getParamDecl(I) == PVD)
      return true;

  return false;
}

// True when the current path already proves len <= capacity: an earlier
// guard rejected larger counts, or the length carries the constraint of a
// clamp. Unprovable lengths keep the alert.
bool SAGenTestChecker::lenIsProvablyBounded(const Expr *LenE,
                                            const llvm::APInt &ArrSize,
                                            CheckerContext &C) const {
  if (!LenE)
    return false;

  ProgramStateRef State = C.getState();
  SValBuilder &SVB = C.getSValBuilder();
  ASTContext &ACtx = C.getASTContext();

  SVal LenV = State->getSVal(LenE, C.getLocationContext());
  auto LenNL = LenV.getAs<NonLoc>();
  if (!LenNL)
    return false;

  llvm::APInt Cap = ArrSize.zextOrTrunc(ACtx.getTypeSize(LenE->getType()));
  NonLoc CapNL = SVB.makeIntVal(Cap, /*isUnsigned=*/true);

  SVal Over = SVB.evalBinOp(State, BO_GT, *LenNL, CapNL, ACtx.IntTy);
  auto OverC = Over.getAs<DefinedOrUnknownSVal>();
  if (!OverC)
    return false;

  // Feasible only on the "len <= capacity" side means the copy is bounded.
  return State->assume(*OverC).first == nullptr;
}

void SAGenTestChecker::reportUnbounded(const CallEvent &Call, const Expr *DestE,
                                       CheckerContext &C, StringRef Msg) const {
  ExplodedNode *EN = C.generateNonFatalErrorNode();
  if (!EN)
    return;

  auto R = std::make_unique<PathSensitiveBugReport>(*BT, Msg, EN);
  R->addRange(Call.getSourceRange());
  if (DestE)
    R->addRange(DestE->getSourceRange());
  C.emitReport(std::move(R));
}

void SAGenTestChecker::checkPreCall(const CallEvent &Call,
                                    CheckerContext &C) const {
  if (!isCopyFromUser(Call, C))
    return;

  if (Call.getNumArgs() < 3)
    return;

  const Expr *DestE = Call.getArgExpr(0);
  const Expr *LenE = Call.getArgExpr(2);
  if (!DestE || !LenE)
    return;

  // Resolve destination array
  const ValueDecl *ArrDecl = nullptr;
  llvm::APInt ArrSize; // number of bytes in array
  if (!getArrayFromDestExpr(DestE, ArrDecl, ArrSize, C))
    return; // only flag when we know it's a fixed-size array

  // If length is a constant, check it against the array size.
  llvm::APSInt EvalRes;
  if (EvaluateExprToInt(EvalRes, LenE, C)) {
    // Negative or zero-length copies are not overflows.
    if (EvalRes.isSigned() && EvalRes.isNegative())
      return;

    llvm::APSInt ArrSizeAPS(EvalRes.getBitWidth(), /*isUnsigned=*/true);
    ArrSizeAPS = ArrSize.getLimitedValue();

    llvm::APSInt LenVal = EvalRes;
    if (LenVal.isSigned())
      LenVal.setIsUnsigned(true); // compare as unsigned

    if (LenVal > ArrSizeAPS) {
      reportUnbounded(Call, DestE, C,
                      "copy_from_user length exceeds destination buffer");
    }
    return; // handled the constant case
  }

  // A bounds guard or clamp earlier on this path already bounds the length
  // by the destination capacity (e.g., an earlier nbytes bound check, or
  // bsize = min(nbytes, sizeof(mybuf) - 1) feeding this call): no overflow.
  if (lenIsProvablyBounded(LenE, ArrSize, C))
    return;

  // Length variable clamped against a fixed-size array whose capacity fits
  // the destination.
  if (const auto *LDRE = dyn_cast<DeclRefExpr>(LenE->IgnoreParenImpCasts())) {
    if (const auto *LVD = dyn_cast<VarDecl>(LDRE->getDecl())) {
      if (const ValueDecl *const *BoundArr =
              C.getState()->get<LenBoundMap>(LVD)) {
        llvm::APInt BoundCap;
        if (getConstantArrayCap(*BoundArr, BoundCap) &&
            BoundCap.getLimitedValue() <= ArrSize.getLimitedValue())
          return;
      }
    }
  }

  // Heuristic: unbounded user length used directly without clamping
  if (lenExprLooksUnboundedUserCount(LenE, ArrDecl, C)) {
    reportUnbounded(Call, DestE, C,
                    "copy_from_user into fixed-size buffer uses unbounded user length");
  }
}

void SAGenTestChecker::checkBind(SVal Loc, SVal Val, const Stmt *S,
                                 CheckerContext &C) const {
  ProgramStateRef State = C.getState();

  const MemRegion *LHSReg = Loc.getAsRegion();
  if (!LHSReg)
    return;
  LHSReg = LHSReg->getBaseRegion();
  if (!LHSReg)
    return;

  const MemRegion *RHSReg = Val.getAsRegion();
  if (!RHSReg)
    return;
  RHSReg = RHSReg->getBaseRegion();
  if (!RHSReg)
    return;

  // We care about aliases to arrays: find if RHS base region is a VarDecl of a fixed array.
  if (const auto *VR = dyn_cast<VarRegion>(RHSReg)) {
    const VarDecl *VD = VR->getDecl();
    if (!VD)
      return;

    // Only care about fixed-size arrays
    if (dyn_cast<ConstantArrayType>(VD->getType().getTypePtr())) {
      // Track alias: pointer (LHSReg) -> array VarDecl (VD)
      State = State->set<BufAliasMap>(LHSReg, VD);
      C.addTransition(State);
    }
  }
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detect unbounded copy_from_user into fixed-size local buffers", "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
