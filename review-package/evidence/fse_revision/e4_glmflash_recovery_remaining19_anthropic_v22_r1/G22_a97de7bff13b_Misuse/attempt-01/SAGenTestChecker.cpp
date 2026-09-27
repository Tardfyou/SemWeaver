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
#include "clang/AST/Stmt.h"
#include "llvm/ADT/APSInt.h"
#include <tuple>

using namespace clang;
using namespace ento;
using namespace taint;

// Track a path-sensitive lower bound for the parameter 'optlen' (if present).
REGISTER_MAP_WITH_PROGRAMSTATE(OptlenLowerBoundMap, const MemRegion *, llvm::APSInt)

namespace {
class SAGenTestChecker : public Checker<
                             check::BeginFunction,
                             check::BranchCondition,
                             check::PostCall> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Setsockopt length validation", "API Misuse")) {}

  void checkBeginFunction(CheckerContext &C) const;
  void checkBranchCondition(const Stmt *Condition, CheckerContext &C) const;
  void checkPostCall(const CallEvent &Call, CheckerContext &C) const;

private:
  // Helper determinations
  static const ParmVarDecl *getLengthParam(const FunctionDecl *FD);
  static const MemRegion *getParmRegion(const ParmVarDecl *PVD, CheckerContext &C);
  static bool isSetSockoptHandler(const FunctionDecl *FD);

  static bool isSockptrCopyCall(const CallEvent &Call, CheckerContext &C,
                                unsigned &SizeArgIndex);
  static bool isSafeBtSockptrCopy(const CallEvent &Call, CheckerContext &C);
  static bool getConstSizeValue(llvm::APSInt &Out, const Expr *E,
                                CheckerContext &C);
  static bool sizeExprMentionsParam(const Expr *E, StringRef ParamName,
                                    CheckerContext &C);

  static ProgramStateRef updateLowerBound(ProgramStateRef St,
                                          const MemRegion *OptlenMR,
                                          const llvm::APSInt &NewLB);

  static BinaryOperator::Opcode invertRelOp(BinaryOperator::Opcode Op);
};

// Discover the user-supplied option length from the enclosing function's own
// declaration: an integer parameter whose name denotes a length. The bound is
// referenced by its discovered name, so consistent renames keep working.
const ParmVarDecl *SAGenTestChecker::getLengthParam(const FunctionDecl *FD) {
  if (!FD)
    return nullptr;
  for (const ParmVarDecl *P : FD->parameters()) {
    if (!P)
      continue;
    if (!P->getType()->isIntegerType())
      continue;
    if (P->getName().contains("len"))
      return P;
  }
  return nullptr;
}

const MemRegion *SAGenTestChecker::getParmRegion(const ParmVarDecl *PVD,
                                                 CheckerContext &C) {
  if (!PVD)
    return nullptr;
  MemRegionManager &MRMgr = C.getSValBuilder().getRegionManager();
  const VarRegion *VR = MRMgr.getVarRegion(PVD, C.getLocationContext());
  if (!VR)
    return nullptr;
  const MemRegion *Base = VR->getBaseRegion();
  return Base;
}

// Structural setsockopt-handler shape: the function performing the copy
// receives the option length as one of its own integer parameters together
// with an option value/name argument. This keeps the report scoped to
// option-handling code without hard-coding any exact function name.
bool SAGenTestChecker::isSetSockoptHandler(const FunctionDecl *FD) {
  if (!FD)
    return false;
  if (!getLengthParam(FD))
    return false;
  for (const ParmVarDecl *P : FD->parameters()) {
    if (!P)
      continue;
    if (P->getName().contains("opt"))
      return true;
  }
  return false;
}

bool SAGenTestChecker::isSafeBtSockptrCopy(const CallEvent &Call,
                                           CheckerContext &C) {
  const Expr *Origin = Call.getOriginExpr();
  if (!Origin)
    return false;
  // Safe helper: bt_copy_from_sockptr(dst, dst_size, src, optlen)
  return ExprHasName(Origin, "bt_copy_from_sockptr", C);
}

bool SAGenTestChecker::isSockptrCopyCall(const CallEvent &Call,
                                         CheckerContext &C,
                                         unsigned &SizeArgIndex) {
  const Expr *Origin = Call.getOriginExpr();
  if (!Origin)
    return false;

  if (ExprHasName(Origin, "copy_from_sockptr_offset", C)) {
    SizeArgIndex = 3; // (dst, src, offset, size)
    return true;
  }
  if (ExprHasName(Origin, "copy_from_sockptr", C)) {
    SizeArgIndex = 2; // (dst, src, size)
    return true;
  }
  return false;
}

bool SAGenTestChecker::getConstSizeValue(llvm::APSInt &Out, const Expr *E,
                                         CheckerContext &C) {
  if (!E)
    return false;
  return EvaluateExprToInt(Out, E, C);
}

bool SAGenTestChecker::sizeExprMentionsParam(const Expr *E, StringRef ParamName,
                                             CheckerContext &C) {
  if (!E || ParamName.empty())
    return false;
  return ExprHasName(E, ParamName, C);
}

ProgramStateRef SAGenTestChecker::updateLowerBound(ProgramStateRef St,
                                                   const MemRegion *OptlenMR,
                                                   const llvm::APSInt &NewLB) {
  if (!St || !OptlenMR)
    return St;

  const llvm::APSInt *Prev = St->get<OptlenLowerBoundMap>(OptlenMR);
  if (!Prev) {
    // No previous LB, set it now.
    return St->set<OptlenLowerBoundMap>(OptlenMR, NewLB);
  }

  // Compare after aligning bit widths and signedness.
  llvm::APSInt PrevAdj = *Prev;
  if (PrevAdj.getBitWidth() != NewLB.getBitWidth())
    PrevAdj = PrevAdj.extOrTrunc(NewLB.getBitWidth());
  PrevAdj.setIsSigned(NewLB.isSigned());

  if (PrevAdj < NewLB) {
    return St->set<OptlenLowerBoundMap>(OptlenMR, NewLB);
  }
  return St;
}

BinaryOperator::Opcode SAGenTestChecker::invertRelOp(BinaryOperator::Opcode Op) {
  switch (Op) {
  case BO_LT:
    return BO_GT;
  case BO_GT:
    return BO_LT;
  case BO_LE:
    return BO_GE;
  case BO_GE:
    return BO_LE;
  case BO_EQ:
    return BO_EQ;
  case BO_NE:
    return BO_NE;
  default:
    return Op;
  }
}

void SAGenTestChecker::checkBeginFunction(CheckerContext &C) const {
  const LocationContext *LCtx = C.getLocationContext();
  const FunctionDecl *FD = dyn_cast_or_null<FunctionDecl>(LCtx->getDecl());
  if (!FD)
    return;

  const ParmVarDecl *OptlenPVD = getLengthParam(FD);
  if (!OptlenPVD)
    return;

  const MemRegion *OptlenMR = getParmRegion(OptlenPVD, C);
  if (!OptlenMR)
    return;

  ProgramStateRef State = C.getState();
  // Clear any previous learning for this param (fresh function entry).
  if (State->contains<OptlenLowerBoundMap>(OptlenMR)) {
    State = State->remove<OptlenLowerBoundMap>(OptlenMR);
    C.addTransition(State);
  }
}

void SAGenTestChecker::checkBranchCondition(const Stmt *Condition,
                                            CheckerContext &C) const {
  const LocationContext *LCtx = C.getLocationContext();
  const FunctionDecl *FD = dyn_cast_or_null<FunctionDecl>(LCtx->getDecl());
  if (!FD)
    return;

  const ParmVarDecl *OptlenPVD = getLengthParam(FD);
  if (!OptlenPVD)
    return;

  const MemRegion *OptlenMR = getParmRegion(OptlenPVD, C);
  if (!OptlenMR)
    return;

  const Expr *CondE = dyn_cast_or_null<Expr>(Condition);
  if (!CondE)
    return;

  CondE = CondE->IgnoreParenImpCasts();
  const auto *BO = dyn_cast<BinaryOperator>(CondE);
  if (!BO)
    return;

  BinaryOperator::Opcode Op = BO->getOpcode();
  // Only relational ops are interesting.
  if (!(Op == BO_LT || Op == BO_LE || Op == BO_GT || Op == BO_GE ||
        Op == BO_EQ || Op == BO_NE))
    return;

  const Expr *LHS = BO->getLHS()->IgnoreParenImpCasts();
  const Expr *RHS = BO->getRHS()->IgnoreParenImpCasts();

  StringRef LenName = OptlenPVD->getName();
  bool LHSHasOptlen = sizeExprMentionsParam(LHS, LenName, C);
  bool RHSHasOptlen = sizeExprMentionsParam(RHS, LenName, C);

  if (LHSHasOptlen == RHSHasOptlen)
    return; // Need exactly one side to reference optlen and the other side to be a constant.

  const Expr *OptlenSide = LHSHasOptlen ? LHS : RHS;
  const Expr *OtherSide = LHSHasOptlen ? RHS : LHS;

  (void)OptlenSide; // Not used beyond presence check.

  llvm::APSInt K;
  if (!getConstSizeValue(K, OtherSide, C))
    return; // We only handle comparisons against constants/sizeof-expr.

  // Normalize to the form: optlen <OpN> K
  BinaryOperator::Opcode OpN = LHSHasOptlen ? Op : invertRelOp(Op);

  // Now split states based on the condition runtime value.
  ProgramStateRef State = C.getState();
  DefinedOrUnknownSVal DV =
      State->getSVal(CondE, C.getLocationContext()).castAs<DefinedOrUnknownSVal>();

  ProgramStateRef StateTrue, StateFalse;
  std::tie(StateTrue, StateFalse) = State->assume(DV);

  // For each branch, update lower bound if we can conclude it.
  // True branch implications:
  if (StateTrue) {
    switch (OpN) {
    case BO_GE:
      StateTrue = updateLowerBound(StateTrue, OptlenMR, K);
      break;
    case BO_GT: {
      llvm::APSInt Kp1 = K;
      Kp1 = Kp1 + 1;
      StateTrue = updateLowerBound(StateTrue, OptlenMR, Kp1);
      break;
    }
    case BO_EQ:
      StateTrue = updateLowerBound(StateTrue, OptlenMR, K);
      break;
    default:
      break; // For <, <=, != true branch does not increase LB.
    }
    C.addTransition(StateTrue);
  }

  // False branch implications:
  if (StateFalse) {
    switch (OpN) {
    case BO_LT:
      // !(optlen < K) => optlen >= K
      StateFalse = updateLowerBound(StateFalse, OptlenMR, K);
      break;
    case BO_LE: {
      // !(optlen <= K) => optlen > K => LB = K+1
      llvm::APSInt Kp1 = K;
      Kp1 = Kp1 + 1;
      StateFalse = updateLowerBound(StateFalse, OptlenMR, Kp1);
      break;
    }
    case BO_NE:
      // !(optlen != K) => optlen == K => LB = K
      StateFalse = updateLowerBound(StateFalse, OptlenMR, K);
      break;
    default:
      break; // For >=, > false branch does not increase LB.
    }
    C.addTransition(StateFalse);
  }
}

void SAGenTestChecker::checkPostCall(const CallEvent &Call,
                                     CheckerContext &C) const {
  const LocationContext *LCtx = C.getLocationContext();
  const FunctionDecl *FD = dyn_cast_or_null<FunctionDecl>(LCtx->getDecl());
  if (!FD || !isSetSockoptHandler(FD))
    return;

  // Safe helper: do not warn.
  if (isSafeBtSockptrCopy(Call, C))
    return;

  unsigned SizeArgIndex = 0;
  if (!isSockptrCopyCall(Call, C, SizeArgIndex))
    return;

  if (SizeArgIndex >= Call.getNumArgs())
    return;

  // Size argument expression
  const Expr *SizeE = Call.getArgExpr(SizeArgIndex);
  if (!SizeE)
    return;

  const ParmVarDecl *OptlenPVD = getLengthParam(FD);
  if (!OptlenPVD)
    return;
  const MemRegion *OptlenMR = getParmRegion(OptlenPVD, C);
  if (!OptlenMR)
    return;

  // If the size expression mentions the option-length parameter itself, the
  // copy is length-aware -> do not warn.
  if (sizeExprMentionsParam(SizeE, OptlenPVD->getName(), C))
    return;

  llvm::APSInt SizeConst;
  if (!getConstSizeValue(SizeConst, SizeE, C))
    return; // Only warn for fixed-size copies (sizeof or literal)

  ProgramStateRef State = C.getState();
  const llvm::APSInt *LBPtr = State->get<OptlenLowerBoundMap>(OptlenMR);

  bool Safe = false;
  if (LBPtr) {
    llvm::APSInt LB = *LBPtr;
    // Align widths and signedness for comparison
    if (LB.getBitWidth() != SizeConst.getBitWidth())
      LB = LB.extOrTrunc(SizeConst.getBitWidth());
    LB.setIsSigned(SizeConst.isSigned());
    // If LB >= SizeConst, it's safe on this path.
    Safe = (LB >= SizeConst);
  }

  if (!Safe) {
    ExplodedNode *N = C.generateNonFatalErrorNode();
    if (!N)
      return;

    auto R = std::make_unique<PathSensitiveBugReport>(
        *BT,
        "copy_from_sockptr uses fixed size without ensuring optlen is large enough",
        N);
    if (SizeE)
      R->addRange(SizeE->getSourceRange());
    C.emitReport(std::move(R));
  }
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detect fixed-size copy_from_sockptr in setsockopt without validating optlen",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
