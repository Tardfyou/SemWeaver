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
#include "llvm/ADT/SmallVector.h"

using namespace clang;
using namespace ento;
using namespace taint;

// Maps the storage location of the firmware output pointer to the symbolic
// return value of the request_firmware() call that populated it.
//
// A null SymbolRef is still meaningful: it means the call was observed, but
// the analyzer did not model its return value symbolically.
REGISTER_MAP_WITH_PROGRAMSTATE(RequestFwMap, const MemRegion *, SymbolRef)

// Avoid emitting repeated reports for the same tracked pointer on one path.
REGISTER_MAP_WITH_PROGRAMSTATE(RequestFwReportedMap, const MemRegion *, bool)

namespace {

class ConditionReferenceVisitor
    : public RecursiveASTVisitor<ConditionReferenceVisitor> {
  llvm::SmallVectorImpl<const Expr *> &References;

public:
  explicit ConditionReferenceVisitor(
      llvm::SmallVectorImpl<const Expr *> &References)
      : References(References) {}

  bool VisitDeclRefExpr(DeclRefExpr *DRE) {
    References.push_back(DRE);
    return true;
  }

  bool VisitMemberExpr(MemberExpr *ME) {
    References.push_back(ME);
    return true;
  }
};

class SAGenTestChecker
    : public Checker<check::PostCall, check::BranchCondition, check::Bind> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Unchecked return value of request_firmware()")) {}

  void checkPostCall(const CallEvent &Call, CheckerContext &C) const;
  void checkBranchCondition(const Stmt *Condition,
                            CheckerContext &C) const;
  void checkBind(SVal Loc, SVal Val, const Stmt *S,
                 CheckerContext &C) const;

private:
  static bool isRequestFirmwareCall(const CallEvent &Call);

  static bool isRequestFirmwareCallExpr(const Stmt *S);

  static const MemRegion *getStorageRegion(const Expr *E,
                                           CheckerContext &C);

  static bool isKnownSuccessfulRequest(SymbolRef ReturnSym,
                                       ProgramStateRef State);
};

bool SAGenTestChecker::isRequestFirmwareCall(const CallEvent &Call) {
  const IdentifierInfo *ID = Call.getCalleeIdentifier();
  return ID && ID->getName() == "request_firmware";
}

bool SAGenTestChecker::isRequestFirmwareCallExpr(const Stmt *S) {
  const auto *CE = dyn_cast_or_null<CallExpr>(S);
  if (!CE)
    return false;

  const FunctionDecl *FD = CE->getDirectCallee();
  return FD && FD->getName() == "request_firmware";
}

const MemRegion *
SAGenTestChecker::getStorageRegion(const Expr *E, CheckerContext &C) {
  if (!E)
    return nullptr;

  E = E->IgnoreParenImpCasts();

  // The first argument of request_firmware() is normally "&fw". We need the
  // storage of fw, not the value stored in fw.
  if (const auto *UO = dyn_cast<UnaryOperator>(E)) {
    if (UO->getOpcode() == UO_AddrOf)
      return getStorageRegion(UO->getSubExpr(), C);
  }

  ProgramStateRef State = C.getState();

  if (const auto *DRE = dyn_cast<DeclRefExpr>(E)) {
    if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
      SVal LValue = State->getLValue(VD, C.getLocationContext());
      return LValue.getAsRegion();
    }
  }

  // Support output pointer fields, such as "&ctx->fw".
  if (const auto *ME = dyn_cast<MemberExpr>(E)) {
    if (const auto *FD = dyn_cast<FieldDecl>(ME->getMemberDecl())) {
      SVal Base = State->getSVal(ME->getBase(), C.getLocationContext());
      SVal LValue = State->getLValue(FD, Base);

      if (const MemRegion *MR = LValue.getAsRegion())
        return MR;
    }
  }

  // Preserve support for storage expressions handled by the analyzer's
  // existing SVal model.
  return getMemRegionFromExpr(E, C);
}

bool SAGenTestChecker::isKnownSuccessfulRequest(SymbolRef ReturnSym,
                                                 ProgramStateRef State) {
  if (!ReturnSym)
    return false;

  const ConstraintManager &CM = State->getConstraintManager();
  const llvm::APSInt *Min = CM.getSymMinVal(State, ReturnSym);
  const llvm::APSInt *Max = CM.getSymMaxVal(State, ReturnSym);

  // request_firmware() succeeds exactly when its integer return value is 0.
  // Both bounds must be zero: a range merely containing zero is insufficient.
  return Min && Max && Min->isZero() && Max->isZero();
}

void SAGenTestChecker::checkPostCall(const CallEvent &Call,
                                     CheckerContext &C) const {
  if (!isRequestFirmwareCall(Call))
    return;

  const Expr *OriginExpr = Call.getOriginExpr();
  const auto *CE = dyn_cast_or_null<CallExpr>(OriginExpr);
  if (!CE || CE->getNumArgs() == 0)
    return;

  const MemRegion *OutputRegion = getStorageRegion(CE->getArg(0), C);
  if (!OutputRegion)
    return;

  ProgramStateRef State = C.getState();

  // The return symbol remains constrained by normal branch processing. For
  // example, after "if (ret) return;", the remaining path has ret == 0.
  SymbolRef ReturnSym = Call.getReturnValue().getAsSymbol();

  State = State->set<RequestFwMap>(OutputRegion, ReturnSym);
  State = State->remove<RequestFwReportedMap>(OutputRegion);
  C.addTransition(State);
}

void SAGenTestChecker::checkBranchCondition(const Stmt *Condition,
                                            CheckerContext &C) const {
  if (!Condition)
    return;

  llvm::SmallVector<const Expr *, 8> References;
  ConditionReferenceVisitor Visitor(References);

  // RecursiveASTVisitor operates on non-const AST nodes but does not mutate
  // the condition during traversal.
  Visitor.TraverseStmt(const_cast<Stmt *>(Condition));

  ProgramStateRef State = C.getState();

  for (const Expr *Reference : References) {
    const MemRegion *StorageRegion = getStorageRegion(Reference, C);
    if (!StorageRegion)
      continue;

    const SymbolRef *ReturnSym = State->get<RequestFwMap>(StorageRegion);
    if (!ReturnSym)
      continue;

    // This is the false-positive fix. A return-value check that dominates the
    // current path proves request_firmware() succeeded before fw is used.
    if (isKnownSuccessfulRequest(*ReturnSym, State))
      continue;

    const bool *AlreadyReported =
        State->get<RequestFwReportedMap>(StorageRegion);
    if (AlreadyReported && *AlreadyReported)
      continue;

    ProgramStateRef ReportState =
        State->set<RequestFwReportedMap>(StorageRegion, true);

    ExplodedNode *N = C.generateNonFatalErrorNode(ReportState);
    if (!N)
      return;

    auto Report = std::make_unique<PathSensitiveBugReport>(
        *BT,
        "Unchecked return value of request_firmware(): "
        "firmware pointer used in condition",
        N);
    Report->addRange(Condition->getSourceRange());
    C.emitReport(std::move(Report));
    return;
  }
}

void SAGenTestChecker::checkBind(SVal Loc, SVal Val, const Stmt *S,
                                 CheckerContext &C) const {
  // request_firmware() itself writes through its first argument. PostCall
  // installs the association after that call, so do not invalidate it here.
  if (isRequestFirmwareCallExpr(S))
    return;

  const MemRegion *StorageRegion = Loc.getAsRegion();
  if (!StorageRegion)
    return;

  ProgramStateRef State = C.getState();

  if (!State->get<RequestFwMap>(StorageRegion) &&
      !State->get<RequestFwReportedMap>(StorageRegion))
    return;

  // A later assignment means the pointer is no longer necessarily the value
  // supplied by the recorded request_firmware() call.
  State = State->remove<RequestFwMap>(StorageRegion);
  State = State->remove<RequestFwReportedMap>(StorageRegion);
  C.addTransition(State);
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects use of request_firmware() output pointers in conditions when "
      "the associated return value has not been proven successful",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
