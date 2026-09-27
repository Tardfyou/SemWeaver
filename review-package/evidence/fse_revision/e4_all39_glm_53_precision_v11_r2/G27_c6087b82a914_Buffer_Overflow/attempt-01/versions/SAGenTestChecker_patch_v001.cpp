#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Checkers/Taint.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/Environment.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramState.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramStateTrait.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ConstraintManager.h"
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

// Program state map: pointer MemRegion -> destination fixed-size array VarDecl
REGISTER_MAP_WITH_PROGRAMSTATE(BufAliasMap, const MemRegion *, const VarDecl *)

namespace {

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
  bool getArrayFromDestExpr(const Expr *Dest, const VarDecl *&ArrVD,
                            llvm::APInt &ArrSize,
                            CheckerContext &C) const;
  bool lengthProvenBounded(SVal LenV, const Expr *LenE,
                           const llvm::APInt &CapBytes,
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

// Byte capacity of a constant-array declaration (element count scaled by the
// element size); false when the declaration is not a constant array.
static bool constantArrayCapacityBytes(const DeclaratorDecl *DD,
                                       const ASTContext &ACtx,
                                       llvm::APInt &Bytes) {
  if (!DD)
    return false;
  if (const auto *CAT =
          dyn_cast<ConstantArrayType>(DD->getType().getTypePtr())) {
    Bytes = CAT->getSize() * (uint64_t)ACtx.getTypeSizeInChars(
                                   CAT->getElementType())
                                 .getQuantity();
    return true;
  }
  return false;
}

// Resolve the destination capacity by walking the region chain upwards, so
// decayed destinations such as &arr[0], arr + i, record->buf or slot[i] still
// reach the underlying fixed-size array declaration.
static bool capacityFromRegionChain(const MemRegion *MR,
                                    const ASTContext &ACtx,
                                    llvm::APInt &Bytes) {
  for (const MemRegion *R = MR; R; R = R->getSuperRegion()) {
    const DeclaratorDecl *DD = nullptr;
    if (const auto *VR = dyn_cast<VarRegion>(R))
      DD = VR->getDecl();
    else if (const auto *FR = dyn_cast<FieldRegion>(R))
      DD = FR->getDecl();
    if (constantArrayCapacityBytes(DD, ACtx, Bytes))
      return true;
  }
  return false;
}

bool SAGenTestChecker::getArrayFromDestExpr(const Expr *Dest,
                                            const VarDecl *&ArrVD,
                                            llvm::APInt &ArrSize,
                                            CheckerContext &C) const {
  ArrVD = nullptr;

  // Strip parentheses and casts before resolving the destination.
  const Expr *DestStripped = Dest->IgnoreParenCasts();

  // Try direct array use: e.g., copy_from_user(mybuf, ...)
  if (const auto *DRE = dyn_cast<DeclRefExpr>(DestStripped)) {
    if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
      if (constantArrayCapacityBytes(VD, C.getASTContext(), ArrSize)) {
        ArrVD = VD;
        return true;
      }
    }
  }

  // Region-based resolution for decayed uses (&arr[0], arr + i, record->buf,
  // slot[i]): walk super-regions to the fixed-size array declaration.
  if (const MemRegion *MR = getMemRegionFromExpr(DestStripped, C)) {
    if (capacityFromRegionChain(MR, C.getASTContext(), ArrSize))
      return true; // ArrVD stays null for field arrays; capacity is known

    // Try alias via program state map: e.g., pbuf = mybuf; copy_from_user(pbuf, ...)
    const MemRegion *Base = MR->getBaseRegion();
    if (!Base)
      return false;

    ProgramStateRef State = C.getState();
    const VarDecl *const *VDPtr = State->get<BufAliasMap>(Base);
    if (!VDPtr || !*VDPtr)
      return false;

    if (constantArrayCapacityBytes(*VDPtr, C.getASTContext(), ArrSize)) {
      ArrVD = *VDPtr;
      return true;
    }
  }

  return false;
}

// True when the copy length value is provably within [0, CapBytes] on the
// current path. Bounds arise from clamps such as
//   bsize = min(nbytes, sizeof(buf) - 1);
// or explicit rewrites "if (n > cap) n = cap;" before the call. After macro
// expansion these become path constraints on the length value, so the proof
// goes through the constraint manager rather than expression spelling.
bool SAGenTestChecker::lengthProvenBounded(SVal LenV, const Expr *LenE,
                                           const llvm::APInt &CapBytes,
                                           CheckerContext &C) const {
  // Constant length: compare directly against the capacity.
  if (auto CI = LenV.getAs<nonloc::ConcreteInt>()) {
    llvm::APSInt Len = CI->getValue();
    if (Len.isSigned() && Len.isNegative())
      return true; // negative lengths do not reach the copy
    llvm::APSInt Cap(Len.getBitWidth(), /*isUnsigned=*/true);
    Cap = CapBytes.getLimitedValue();
    Len.setIsUnsigned(true);
    return Len <= Cap;
  }

  SymbolRef Sym = LenV.getAsSymbol();
  if (!Sym)
    return false; // unknown length: cannot prove any bound

  ProgramStateRef State = C.getState();
  ProgramStateManager &SM = State->getStateManager();
  ConstraintManager &CM = SM.getConstraintManager();

  // A length fully pinned to a single value by an earlier equality check.
  if (const llvm::APSInt *Pinned = CM.getSymVal(State, Sym)) {
    llvm::APSInt Len = *Pinned;
    if (Len.isSigned() && Len.isNegative())
      return true;
    llvm::APSInt Cap(Len.getBitWidth(), /*isUnsigned=*/true);
    Cap = CapBytes.getLimitedValue();
    Len.setIsUnsigned(true);
    return Len <= Cap;
  }

  // Otherwise ask whether the length can fall outside [0, capacity]. If the
  // out-of-range assumption is infeasible, every path reaching this call is
  // bounded by the destination capacity.
  unsigned Bits = 64;
  if (LenE) {
    unsigned TypeBits = (unsigned)C.getASTContext().getTypeSize(
        LenE->IgnoreParenImpCasts()->getType());
    if (TypeBits > 0)
      Bits = TypeBits;
  }
  llvm::APSInt From(Bits, /*isUnsigned=*/true);
  llvm::APSInt To(Bits, /*isUnsigned=*/true);
  To = CapBytes.getLimitedValue();
  nonloc::SymbolVal LenSym(Sym);
  auto Outside = CM.assumeInclusiveRange(State, LenSym, From, To,
                                          /*InRange=*/false);
  if (!Outside.first && Outside.second)
    return true; // provably inside [0, capacity]
  return false;
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
  const VarDecl *ArrVD = nullptr;
  llvm::APInt ArrSize; // number of bytes in array
  if (!getArrayFromDestExpr(DestE, ArrVD, ArrSize, C))
    return; // only flag when we know it's a fixed-size local array

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

  // Symbolic length: warn only when the length is not provably bounded by
  // the destination capacity on this path. Clamps such as
  // bsize = min(nbytes, sizeof(mybuf) - 1) or an explicit
  // "if (n > cap) n = cap;" rewrite constrain the length value in the path
  // state, so bounded copies stay silent while unconstrained user-supplied
  // lengths still warn.
  if (!lengthProvenBounded(Call.getArgSVal(2), LenE, ArrSize, C)) {
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
