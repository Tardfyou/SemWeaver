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
  bool isCopyFromUser(const CallEvent &Call, CheckerContext &C) const;
  bool getArrayFromDestExpr(const Expr *Dest, const VarDecl *&ArrVD,
                            llvm::APInt &ArrSize,
                            CheckerContext &C) const;
  bool lenExprLooksUnboundedUserCount(const Expr *LenE,
                                      CheckerContext &C) const;
  bool isLengthProvablyWithinArray(const CallEvent &Call,
                                   const llvm::APInt &ArrSize,
                                   CheckerContext &C) const;
  void reportUnbounded(const CallEvent &Call, const Expr *DestE,
                       CheckerContext &C, StringRef Msg) const;
};

bool SAGenTestChecker::isCopyFromUser(const CallEvent &Call,
                                      CheckerContext &C) const {
  const Expr *Origin = Call.getOriginExpr();
  if (!Origin)
    return false;

  if (ExprHasName(Origin, "copy_from_user", C))
    return true;
  if (ExprHasName(Origin, "__copy_from_user", C))
    return true;
  return false;
}

bool SAGenTestChecker::getArrayFromDestExpr(const Expr *Dest,
                                            const VarDecl *&ArrVD,
                                            llvm::APInt &ArrSize,
                                            CheckerContext &C) const {
  ArrVD = nullptr;
  if (!Dest)
    return false;

  // Try direct array use: e.g., copy_from_user(mybuf, ...)
  if (const auto *DRE =
          dyn_cast<DeclRefExpr>(Dest->IgnoreParenImpCasts())) {
    if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
      if (const auto *CAT =
              dyn_cast<ConstantArrayType>(VD->getType().getTypePtr())) {
        ArrVD = VD;
        ArrSize = CAT->getSize();
        return true;
      }
    }
  }

  // Try alias via program state map: e.g., pbuf = mybuf; copy_from_user(pbuf, ...)
  const MemRegion *MR = getMemRegionFromExpr(Dest, C);
  if (!MR)
    return false;
  MR = MR->getBaseRegion();
  if (!MR)
    return false;

  ProgramStateRef State = C.getState();
  const VarDecl *const *VDPtr = State->get<BufAliasMap>(MR);
  if (!VDPtr || !*VDPtr)
    return false;

  const VarDecl *VD = *VDPtr;
  if (const auto *CAT =
          dyn_cast<ConstantArrayType>(VD->getType().getTypePtr())) {
    ArrVD = VD;
    ArrSize = CAT->getSize();
    return true;
  }

  return false;
}

bool SAGenTestChecker::lenExprLooksUnboundedUserCount(
    const Expr *LenE, CheckerContext &C) const {
  if (!LenE)
    return false;

  // Direct parameter reference named "nbytes" or "count".
  if (const auto *DRE =
          dyn_cast<DeclRefExpr>(LenE->IgnoreParenImpCasts())) {
    if (const auto *PVD = dyn_cast<ParmVarDecl>(DRE->getDecl())) {
      StringRef Name = PVD->getName();
      if (Name.equals("nbytes") || Name.equals("count"))
        return true;
    }
  }

  // Retain support for expressions involving common user-count names. This
  // heuristic is used only when the analyzer cannot prove the length is safe.
  return ExprHasName(LenE, "nbytes", C) || ExprHasName(LenE, "count", C);
}

bool SAGenTestChecker::isLengthProvablyWithinArray(
    const CallEvent &Call, const llvm::APInt &ArrSize,
    CheckerContext &C) const {
  if (Call.getNumArgs() < 3)
    return false;

  SVal LenVal = Call.getArgSVal(2);
  SymbolRef LenSym = LenVal.getAsSymbol();
  if (!LenSym)
    return false;

  const llvm::APSInt *MaxVal = inferSymbolMaxVal(LenSym, C);
  if (!MaxVal)
    return false;

  // copy_from_user's length is a size_t in the target pattern. Treat a
  // negative signed upper bound as non-overflowing rather than converting it
  // into a large unsigned value.
  if (MaxVal->isSigned() && MaxVal->isNegative())
    return true;

  const llvm::APInt &MaxInt = *MaxVal;
  unsigned Width = std::max(MaxInt.getBitWidth(), ArrSize.getBitWidth());
  llvm::APInt ExtendedMax = MaxInt.zextOrTrunc(Width);
  llvm::APInt ExtendedArrSize = ArrSize.zextOrTrunc(Width);

  return ExtendedMax.ule(ExtendedArrSize);
}

void SAGenTestChecker::reportUnbounded(const CallEvent &Call,
                                       const Expr *DestE,
                                       CheckerContext &C,
                                       StringRef Msg) const {
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
  if (!isCopyFromUser(Call, C) || Call.getNumArgs() < 3)
    return;

  const Expr *DestE = Call.getArgExpr(0);
  const Expr *LenE = Call.getArgExpr(2);
  if (!DestE || !LenE)
    return;

  const VarDecl *ArrVD = nullptr;
  llvm::APInt ArrSize;
  if (!getArrayFromDestExpr(DestE, ArrVD, ArrSize, C))
    return;

  // Check constants first, including lengths that are constant after
  // evaluating casts and other implicit expression wrappers.
  llvm::APSInt EvalRes;
  if (EvaluateExprToInt(EvalRes, LenE, C)) {
    if (EvalRes.isSigned() && EvalRes.isNegative())
      return;

    llvm::APInt LenInt = EvalRes;
    unsigned Width = std::max(LenInt.getBitWidth(), ArrSize.getBitWidth());
    llvm::APInt ExtendedLen = LenInt.zextOrTrunc(Width);
    llvm::APInt ExtendedArrSize = ArrSize.zextOrTrunc(Width);

    if (ExtendedLen.ugt(ExtendedArrSize)) {
      reportUnbounded(Call, DestE, C,
                      "copy_from_user length exceeds destination buffer");
    }
    return;
  }

  // Respect path constraints. For example, after the false branch of
  // `if (nbytes > sizeof(buf) - 1) nbytes = sizeof(buf) - 1`, the analyzer
  // knows the current value of nbytes cannot exceed the array size.
  if (isLengthProvablyWithinArray(Call, ArrSize, C))
    return;

  if (lenExprLooksUnboundedUserCount(LenE, C)) {
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

  // Track aliases whose source is a fixed-size array.
  if (const auto *VR = dyn_cast<VarRegion>(RHSReg)) {
    const VarDecl *VD = VR->getDecl();
    if (!VD)
      return;

    if (dyn_cast<ConstantArrayType>(VD->getType().getTypePtr())) {
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
```