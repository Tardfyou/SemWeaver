#include "clang/AST/Stmt.h"
#include "clang/Lex/Lexer.h"
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
#include "clang/AST/ASTContext.h"

using namespace clang;
using namespace ento;
using namespace taint;

namespace {

enum class FailureBranch {
  None,
  Then,
  Else
};

struct ManagedActionRegistration {
  const CallExpr *RegistrationCall = nullptr;
  const FunctionDecl *CleanupDecl = nullptr;
};

static const Expr *stripParenAndImplicitCasts(const Expr *E) {
  return E ? E->IgnoreParenImpCasts() : nullptr;
}

static const FunctionDecl *getCanonicalFunctionDecl(const FunctionDecl *FD) {
  return FD ? FD->getCanonicalDecl() : nullptr;
}

static const FunctionDecl *getDirectFunctionDecl(const CallExpr *CE) {
  return CE ? CE->getDirectCallee() : nullptr;
}

static bool isDevmAddActionOrReset(const CallExpr *CE) {
  const FunctionDecl *FD = getDirectFunctionDecl(CE);
  return FD && FD->getName() == "devm_add_action_or_reset";
}

static const FunctionDecl *getCleanupCallbackDecl(const CallExpr *CE) {
  if (!CE || CE->getNumArgs() < 2)
    return nullptr;

  const Expr *Callback = stripParenAndImplicitCasts(CE->getArg(1));
  if (!Callback)
    return nullptr;

  // Accept both "cleanup" and "&cleanup".
  if (const auto *UO = dyn_cast<UnaryOperator>(Callback)) {
    if (UO->getOpcode() != UO_AddrOf)
      return nullptr;
    Callback = stripParenAndImplicitCasts(UO->getSubExpr());
  }

  const auto *DRE = dyn_cast_or_null<DeclRefExpr>(Callback);
  if (!DRE)
    return nullptr;

  return dyn_cast<FunctionDecl>(DRE->getDecl());
}

class ManagedActionConditionVisitor
    : public RecursiveASTVisitor<ManagedActionConditionVisitor> {
  unsigned RegistrationCount = 0;
  bool Ambiguous = false;
  ManagedActionRegistration Result;

public:
  bool VisitCallExpr(CallExpr *CE) {
    if (!isDevmAddActionOrReset(CE))
      return true;

    ++RegistrationCount;
    const FunctionDecl *Callback = getCleanupCallbackDecl(CE);

    // A condition containing more than one registration is intentionally not
    // modeled. It is unclear which registration owns a later cleanup call.
    if (RegistrationCount != 1 || !Callback) {
      Ambiguous = true;
      return true;
    }

    Result.RegistrationCall = CE;
    Result.CleanupDecl = getCanonicalFunctionDecl(Callback);
    return true;
  }

  bool getSingleRegistration(ManagedActionRegistration &Out) const {
    if (Ambiguous || RegistrationCount != 1 || !Result.RegistrationCall ||
        !Result.CleanupDecl)
      return false;

    Out = Result;
    return true;
  }
};

static bool getManagedActionRegistration(
    const Expr *Condition, ManagedActionRegistration &Registration) {
  if (!Condition)
    return false;

  ManagedActionConditionVisitor Visitor;
  Visitor.TraverseStmt(const_cast<Expr *>(Condition));
  return Visitor.getSingleRegistration(Registration);
}

static bool isRegistrationExpr(const Expr *E,
                               const CallExpr *RegistrationCall) {
  return stripParenAndImplicitCasts(E) == RegistrationCall;
}

static bool isZeroIntegerLiteral(const Expr *E) {
  const auto *IL =
      dyn_cast_or_null<IntegerLiteral>(stripParenAndImplicitCasts(E));
  return IL && IL->getValue().isZero();
}

// devm_add_action_or_reset() returns zero on success and invokes its reset
// action before returning an error. Determine which if branch is its error path.
static FailureBranch getFailureBranch(const Expr *Condition,
                                      const CallExpr *RegistrationCall) {
  const Expr *Cond = stripParenAndImplicitCasts(Condition);
  if (!Cond)
    return FailureBranch::None;

  // if (devm_add_action_or_reset(...))
  if (isRegistrationExpr(Cond, RegistrationCall))
    return FailureBranch::Then;

  // if (!devm_add_action_or_reset(...))
  if (const auto *UO = dyn_cast<UnaryOperator>(Cond)) {
    if (UO->getOpcode() == UO_LNot &&
        isRegistrationExpr(UO->getSubExpr(), RegistrationCall))
      return FailureBranch::Else;
  }

  const auto *BO = dyn_cast<BinaryOperator>(Cond);
  if (!BO)
    return FailureBranch::None;

  // if ((ret = devm_add_action_or_reset(...)))
  if (BO->getOpcode() == BO_Assign &&
      isRegistrationExpr(BO->getRHS(), RegistrationCall))
    return FailureBranch::Then;

  const bool RegistrationOnLHS =
      isRegistrationExpr(BO->getLHS(), RegistrationCall) &&
      isZeroIntegerLiteral(BO->getRHS());
  const bool RegistrationOnRHS =
      isZeroIntegerLiteral(BO->getLHS()) &&
      isRegistrationExpr(BO->getRHS(), RegistrationCall);

  if (!RegistrationOnLHS && !RegistrationOnRHS)
    return FailureBranch::None;

  // Both "call != 0" and "0 != call" enter the then branch on failure.
  if (BO->getOpcode() == BO_NE)
    return FailureBranch::Then;

  // Both "call == 0" and "0 == call" enter the else branch on failure.
  if (BO->getOpcode() == BO_EQ)
    return FailureBranch::Else;

  return FailureBranch::None;
}

static FailureBranch getBranchContainingStmt(const IfStmt *If,
                                             const Stmt *Child) {
  if (!If || !Child)
    return FailureBranch::None;

  if (If->getThen() == Child)
    return FailureBranch::Then;

  if (If->getElse() == Child)
    return FailureBranch::Else;

  return FailureBranch::None;
}

static bool isSameCleanupData(const CallExpr *RegistrationCall,
                              const CallExpr *ManualCleanupCall,
                              CheckerContext &C) {
  // devm_add_action_or_reset(dev, action, data)
  // action(data)
  if (!RegistrationCall || !ManualCleanupCall ||
      RegistrationCall->getNumArgs() < 3 ||
      ManualCleanupCall->getNumArgs() < 1)
    return false;

  ProgramStateRef State = C.getState();
  SVal RegisteredData = State->getSVal(RegistrationCall->getArg(2),
                                       C.getLocationContext());
  SVal ManualCleanupData = State->getSVal(ManualCleanupCall->getArg(0),
                                          C.getLocationContext());

  // Do not infer ownership equivalence from unknown or undefined values.
  if (RegisteredData.isUnknownOrUndef() ||
      ManualCleanupData.isUnknownOrUndef())
    return false;

  if (RegisteredData == ManualCleanupData)
    return true;

  // Pointer casts can produce distinct SVals while still referring to the
  // same memory region.
  const MemRegion *RegisteredRegion = RegisteredData.getAsRegion();
  const MemRegion *ManualCleanupRegion = ManualCleanupData.getAsRegion();

  return RegisteredRegion && ManualCleanupRegion &&
         RegisteredRegion == ManualCleanupRegion;
}

class SAGenTestChecker : public Checker<check::PreCall> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Redundant Cleanup Call", "Double Free")) {}

  void checkPreCall(const CallEvent &Call, CheckerContext &C) const;

private:
  bool isRedundantCleanupAfterReset(const CallExpr *ManualCleanupCall,
                                    const FunctionDecl *ManualCleanupDecl,
                                    CheckerContext &C) const;

  void reportRedundantCleanup(const CallEvent &Call,
                              CheckerContext &C) const;
};

bool SAGenTestChecker::isRedundantCleanupAfterReset(
    const CallExpr *ManualCleanupCall, const FunctionDecl *ManualCleanupDecl,
    CheckerContext &C) const {
  const Stmt *Child = ManualCleanupCall;
  const FunctionDecl *CanonicalManualCleanup =
      getCanonicalFunctionDecl(ManualCleanupDecl);

  while (Child) {
    const auto Parents = C.getASTContext().getParents(*Child);
    const Stmt *Parent = nullptr;

    for (const DynTypedNode &Node : Parents) {
      if (const auto *ParentStmt = Node.get<Stmt>()) {
        Parent = ParentStmt;
        break;
      }
    }

    if (!Parent)
      return false;

    if (const auto *If = dyn_cast<IfStmt>(Parent)) {
      const FailureBranch CurrentBranch = getBranchContainingStmt(If, Child);

      if (CurrentBranch != FailureBranch::None) {
        ManagedActionRegistration Registration;

        if (getManagedActionRegistration(If->getCond(), Registration) &&
            Registration.CleanupDecl == CanonicalManualCleanup &&
            getFailureBranch(If->getCond(), Registration.RegistrationCall) ==
                CurrentBranch &&
            isSameCleanupData(Registration.RegistrationCall,
                              ManualCleanupCall, C)) {
          return true;
        }
      }
    }

    Child = Parent;
  }

  return false;
}

void SAGenTestChecker::checkPreCall(const CallEvent &Call,
                                    CheckerContext &C) const {
  const Expr *OriginExpr = Call.getOriginExpr();
  const auto *ManualCleanupCall =
      dyn_cast_or_null<CallExpr>(stripParenAndImplicitCasts(OriginExpr));
  if (!ManualCleanupCall)
    return;

  const FunctionDecl *ManualCleanupDecl =
      getDirectFunctionDecl(ManualCleanupCall);
  if (!ManualCleanupDecl)
    return;

  // This excludes devm_add_action_or_reset() itself. Its callback argument is
  // a DeclRefExpr, not a direct invocation of the cleanup function.
  if (!isRedundantCleanupAfterReset(ManualCleanupCall, ManualCleanupDecl, C))
    return;

  reportRedundantCleanup(Call, C);
}

void SAGenTestChecker::reportRedundantCleanup(const CallEvent &Call,
                                              CheckerContext &C) const {
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT, "Redundant cleanup call leads to double free", N);
  Report->addRange(Call.getSourceRange());
  C.emitReport(std::move(Report));
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects redundant cleanup call in error handling leading to double free",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
