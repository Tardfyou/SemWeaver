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
#include "clang/Lex/Lexer.h"

using namespace clang;
using namespace ento;
using namespace taint;

// Maps the exact storage region passed as request_firmware()'s first argument
// to the symbolic result returned by that request_firmware() call.
REGISTER_MAP_WITH_PROGRAMSTATE(RequestFwMap, const MemRegion *, SymbolRef)

// Contains all symbolic return values produced by request_firmware().
REGISTER_SET_WITH_PROGRAMSTATE(RequestFwResults, SymbolRef)

// Contains request_firmware() results proven to be zero on the current path.
REGISTER_SET_WITH_PROGRAMSTATE(VerifiedRequestFwResults, SymbolRef)

// Maps a branch-condition symbol to the request_firmware() return symbol that
// the condition tests.
REGISTER_MAP_WITH_PROGRAMSTATE(RequestFwConditionResultMap, SymbolRef,
                               SymbolRef)

// For each tracked branch condition, records whether taking the true branch
// proves that the corresponding request_firmware() result is successful.
REGISTER_MAP_WITH_PROGRAMSTATE(RequestFwSuccessOnTrueMap, SymbolRef, bool)

namespace {

class SAGenTestChecker
    : public Checker<check::PostCall, check::BranchCondition, check::Bind,
                     check::eval::Assume> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this,
                       "Unchecked return value of request_firmware()")) {}

  void checkPostCall(const CallEvent &Call, CheckerContext &C) const;
  void checkBranchCondition(const Stmt *Condition,
                            CheckerContext &C) const;
  void checkBind(SVal Loc, SVal Val, const Stmt *S,
                 CheckerContext &C) const;

  ProgramStateRef evalAssume(ProgramStateRef State, SVal Cond,
                             bool Assumption) const;

private:
  static bool isRequestFirmwareCall(const CallEvent &Call);

  static const MemRegion *getOutputPointerRegion(const Expr *E,
                                                  CheckerContext &C);

  static const MemRegion *getDirectPointerRegion(const Expr *E,
                                                  CheckerContext &C);

  static bool isNullPointerConstant(const Expr *E, CheckerContext &C);

  static bool isIntegerZero(const Expr *E, CheckerContext &C);

  static SymbolRef getTrackedRequestResult(const Expr *E,
                                           CheckerContext &C);

  static bool getRequestResultCheck(const Expr *Condition,
                                    CheckerContext &C,
                                    SymbolRef &Result,
                                    bool &SuccessOnTrue);

  static const MemRegion *findDirectTrackedPointerCheck(
      const Expr *Condition, ProgramStateRef State, CheckerContext &C);
};

bool SAGenTestChecker::isRequestFirmwareCall(const CallEvent &Call) {
  const IdentifierInfo *Callee = Call.getCalleeIdentifier();
  return Callee && Callee->getName() == "request_firmware";
}

const MemRegion *
SAGenTestChecker::getOutputPointerRegion(const Expr *E,
                                         CheckerContext &C) {
  if (!E)
    return nullptr;

  ProgramStateRef State = C.getState();
  return State->getSVal(E, C.getLocationContext()).getAsRegion();
}

const MemRegion *
SAGenTestChecker::getDirectPointerRegion(const Expr *E,
                                         CheckerContext &C) {
  if (!E)
    return nullptr;

  E = E->IgnoreParenImpCasts();

  // A direct pointer condition must operate on a pointer-valued lvalue, such
  // as "fw" or "obj->fw". This intentionally excludes "fw->size".
  if (!E->isGLValue() || !E->getType()->isPointerType())
    return nullptr;

  ProgramStateRef State = C.getState();
  return State->getSVal(E, C.getLocationContext()).getAsRegion();
}

bool SAGenTestChecker::isNullPointerConstant(const Expr *E,
                                              CheckerContext &C) {
  if (!E)
    return false;

  return E->isNullPointerConstant(
      C.getASTContext(), Expr::NPC_ValueDependentIsNotNull);
}

bool SAGenTestChecker::isIntegerZero(const Expr *E, CheckerContext &C) {
  if (!E)
    return false;

  Expr::EvalResult Result;
  if (!E->EvaluateAsInt(Result, C.getASTContext()))
    return false;

  return Result.Val.getInt().isZero();
}

SymbolRef SAGenTestChecker::getTrackedRequestResult(const Expr *E,
                                                     CheckerContext &C) {
  if (!E)
    return nullptr;

  ProgramStateRef State = C.getState();
  SVal Value = State->getSVal(E, C.getLocationContext());
  SymbolRef Sym = Value.getAsSymbol();

  if (!Sym || !State->contains<RequestFwResults>(Sym))
    return nullptr;

  return Sym;
}

bool SAGenTestChecker::getRequestResultCheck(const Expr *Condition,
                                              CheckerContext &C,
                                              SymbolRef &Result,
                                              bool &SuccessOnTrue) {
  if (!Condition)
    return false;

  const Expr *E = Condition->IgnoreParenImpCasts();

  // if (ret): the false branch proves ret == 0.
  if ((Result = getTrackedRequestResult(Condition, C))) {
    SuccessOnTrue = false;
    return true;
  }

  // if (!ret): the true branch proves ret == 0.
  if (const auto *UO = dyn_cast<UnaryOperator>(E)) {
    if (UO->getOpcode() == UO_LNot) {
      Result = getTrackedRequestResult(UO->getSubExpr(), C);
      if (Result) {
        SuccessOnTrue = true;
        return true;
      }
    }

    return false;
  }

  const auto *BO = dyn_cast<BinaryOperator>(E);
  if (!BO)
    return false;

  const Expr *LHS = BO->getLHS();
  const Expr *RHS = BO->getRHS();

  SymbolRef LHSSym = getTrackedRequestResult(LHS, C);
  SymbolRef RHSSym = getTrackedRequestResult(RHS, C);

  const bool RHSIsZero =
      isIntegerZero(RHS, C) || isNullPointerConstant(RHS, C);
  const bool LHSIsZero =
      isIntegerZero(LHS, C) || isNullPointerConstant(LHS, C);

  // ret == 0 and ret != 0, including reversed forms such as 0 == ret.
  if ((LHSSym && RHSIsZero) || (RHSSym && LHSIsZero)) {
    Result = LHSSym ? LHSSym : RHSSym;

    switch (BO->getOpcode()) {
    case BO_EQ:
      SuccessOnTrue = true;
      return true;
    case BO_NE:
      SuccessOnTrue = false;
      return true;
    default:
      break;
    }
  }

  // request_firmware() returns 0 on success and a negative errno on failure.
  // Therefore, "ret < 0" is an error branch and "ret >= 0" is a success
  // branch. Do not infer success from ret <= 0 or ret > 0.
  if (LHSSym && RHSIsZero) {
    Result = LHSSym;

    switch (BO->getOpcode()) {
    case BO_LT:
      SuccessOnTrue = false;
      return true;
    case BO_GE:
      SuccessOnTrue = true;
      return true;
    default:
      break;
    }
  }

  if (RHSSym && LHSIsZero) {
    Result = RHSSym;

    switch (BO->getOpcode()) {
    case BO_GT: // 0 > ret is equivalent to ret < 0.
      SuccessOnTrue = false;
      return true;
    case BO_LE: // 0 <= ret is equivalent to ret >= 0.
      SuccessOnTrue = true;
      return true;
    default:
      break;
    }
  }

  return false;
}

const MemRegion *SAGenTestChecker::findDirectTrackedPointerCheck(
    const Expr *Condition, ProgramStateRef State, CheckerContext &C) {
  if (!Condition)
    return nullptr;

  const Expr *E = Condition->IgnoreParenImpCasts();

  // Handle if (!fw), including nested parentheses and implicit casts.
  if (const auto *UO = dyn_cast<UnaryOperator>(E)) {
    if (UO->getOpcode() == UO_LNot)
      return findDirectTrackedPointerCheck(UO->getSubExpr(), State, C);
  }

  if (const auto *BO = dyn_cast<BinaryOperator>(E)) {
    // Preserve detection for conditions such as:
    //   if (!fw || fallback)
    //   if (ready && !fw)
    if (BO->getOpcode() == BO_LAnd || BO->getOpcode() == BO_LOr) {
      if (const MemRegion *MR =
              findDirectTrackedPointerCheck(BO->getLHS(), State, C))
        return MR;

      return findDirectTrackedPointerCheck(BO->getRHS(), State, C);
    }

    // Handle direct null comparisons:
    //   if (fw == NULL)
    //   if (NULL != fw)
    if (BO->getOpcode() == BO_EQ || BO->getOpcode() == BO_NE) {
      const MemRegion *LHSRegion = getDirectPointerRegion(BO->getLHS(), C);
      const MemRegion *RHSRegion = getDirectPointerRegion(BO->getRHS(), C);

      if (LHSRegion && isNullPointerConstant(BO->getRHS(), C) &&
          State->get<RequestFwMap>(LHSRegion))
        return LHSRegion;

      if (RHSRegion && isNullPointerConstant(BO->getLHS(), C) &&
          State->get<RequestFwMap>(RHSRegion))
        return RHSRegion;
    }

    return nullptr;
  }

  // Handle if (fw). This is a direct pointer truth-value check.
  if (const MemRegion *MR = getDirectPointerRegion(E, C)) {
    if (State->get<RequestFwMap>(MR))
      return MR;
  }

  return nullptr;
}

void SAGenTestChecker::checkPostCall(const CallEvent &Call,
                                     CheckerContext &C) const {
  if (!isRequestFirmwareCall(Call))
    return;

  if (Call.getNumArgs() < 1)
    return;

  const Expr *FirstArg = Call.getArgExpr(0);
  const MemRegion *OutputRegion = getOutputPointerRegion(FirstArg, C);
  if (!OutputRegion)
    return;

  // request_firmware() returns an int status. Associate that precise symbolic
  // result with the pointer storage passed through its first argument.
  SymbolRef Result = Call.getReturnValue().getAsSymbol();
  if (!Result)
    return;

  ProgramStateRef State = C.getState();
  State = State->set<RequestFwMap>(OutputRegion, Result);
  State = State->add<RequestFwResults>(Result);
  C.addTransition(State);
}

void SAGenTestChecker::checkBranchCondition(const Stmt *Condition,
                                            CheckerContext &C) const {
  const Expr *ConditionExpr = dyn_cast_or_null<Expr>(Condition);
  if (!ConditionExpr)
    return;

  ProgramStateRef State = C.getState();

  // Record how the branch outcome relates to request_firmware() success.
  SymbolRef Result;
  bool SuccessOnTrue = false;
  if (getRequestResultCheck(ConditionExpr, C, Result, SuccessOnTrue)) {
    SVal ConditionValue =
        State->getSVal(ConditionExpr, C.getLocationContext());
    SymbolRef ConditionSym = ConditionValue.getAsSymbol();

    if (ConditionSym) {
      State =
          State->set<RequestFwConditionResultMap>(ConditionSym, Result);
      State =
          State->set<RequestFwSuccessOnTrueMap>(ConditionSym, SuccessOnTrue);
    }
  }

  // Only direct pointer truth/null checks are reportable. In particular,
  // fw->size is not a test of fw itself and must never match this pattern.
  const MemRegion *PointerRegion =
      findDirectTrackedPointerCheck(ConditionExpr, State, C);
  if (!PointerRegion) {
    C.addTransition(State);
    return;
  }

  const SymbolRef *ResultForPointer =
      State->get<RequestFwMap>(PointerRegion);
  if (!ResultForPointer) {
    C.addTransition(State);
    return;
  }

  // On a path where ret == 0 has already been established, request_firmware()
  // guarantees that the output pointer is valid. A later pointer check is
  // redundant but not this bug pattern.
  if (State->contains<VerifiedRequestFwResults>(*ResultForPointer)) {
    C.addTransition(State);
    return;
  }

  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT,
      "Unchecked return value of request_firmware(): firmware pointer "
      "used in condition",
      N);
  Report->addRange(ConditionExpr->getSourceRange());
  C.emitReport(std::move(Report));

  // Avoid duplicate diagnostics for the same unverified output pointer on the
  // continuing path.
  State = State->remove<RequestFwMap>(PointerRegion);
  C.addTransition(State);
}

ProgramStateRef SAGenTestChecker::evalAssume(ProgramStateRef State,
                                             SVal Cond,
                                             bool Assumption) const {
  SymbolRef ConditionSym = Cond.getAsSymbol();
  if (!ConditionSym)
    return State;

  const SymbolRef *Result =
      State->get<RequestFwConditionResultMap>(ConditionSym);
  const bool *SuccessOnTrue =
      State->get<RequestFwSuccessOnTrueMap>(ConditionSym);

  if (!Result || !SuccessOnTrue)
    return State;

  State = State->remove<RequestFwConditionResultMap>(ConditionSym);
  State = State->remove<RequestFwSuccessOnTrueMap>(ConditionSym);

  // Mark the result verified only on the branch that establishes ret == 0.
  if (Assumption == *SuccessOnTrue)
    State = State->add<VerifiedRequestFwResults>(*Result);

  return State;
}

void SAGenTestChecker::checkBind(SVal Loc, SVal Val, const Stmt *S,
                                 CheckerContext &C) const {
  const MemRegion *Region = Loc.getAsRegion();
  if (!Region)
    return;

  ProgramStateRef State = C.getState();
  if (!State->get<RequestFwMap>(Region))
    return;

  // A later assignment to fw means it no longer necessarily holds the output
  // of the request_firmware() call tracked above.
  State = State->remove<RequestFwMap>(Region);
  C.addTransition(State);
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects direct firmware-pointer checks when request_firmware() return "
      "status was not verified",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
