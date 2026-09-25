// /external/upstream-study/SAGEN/result-0224-bugfail-multi-o3mini/test-Double-Free-39b13dce1a91cdfc3bec9238f9e89094551bd428/checkers/checker0.cpp
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "clang/Lex/Lexer.h"
#include "clang/StaticAnalyzer/Checkers/utility.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include "llvm/ADT/StringRef.h"

using namespace clang;
using namespace ento;

namespace {

static const FunctionDecl *canonicalFunction(const FunctionDecl *FD) {
  if (!FD)
    return nullptr;
  return FD->getCanonicalDecl();
}

static const FunctionDecl *getReferencedFunction(const Expr *E) {
  if (!E)
    return nullptr;

  E = E->IgnoreParenImpCasts();
  if (const auto *UO = dyn_cast<UnaryOperator>(E)) {
    if (UO->getOpcode() == UO_AddrOf)
      E = UO->getSubExpr()->IgnoreParenImpCasts();
  }

  if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
    return canonicalFunction(dyn_cast<FunctionDecl>(DRE->getDecl()));

  return nullptr;
}

static bool isDevmAddActionOrReset(const CallExpr *CE) {
  if (!CE || CE->getNumArgs() < 3)
    return false;

  const FunctionDecl *FD = CE->getDirectCallee();
  return FD && FD->getName() == "devm_add_action_or_reset";
}

static const CallExpr *findDevmAddActionOrResetCall(const Stmt *S) {
  if (!S)
    return nullptr;

  if (const auto *CE = dyn_cast<CallExpr>(S)) {
    if (isDevmAddActionOrReset(CE))
      return CE;
  }

  for (const Stmt *Child : S->children()) {
    if (const CallExpr *Found = findDevmAddActionOrResetCall(Child))
      return Found;
  }
  return nullptr;
}

static bool stmtContains(const Stmt *Root, const Stmt *Needle) {
  if (!Root || !Needle)
    return false;
  if (Root == Needle)
    return true;

  for (const Stmt *Child : Root->children()) {
    if (stmtContains(Child, Needle))
      return true;
  }
  return false;
}

static StringRef getExprText(const Expr *E, CheckerContext &C) {
  if (!E)
    return StringRef();

  const SourceManager &SM = C.getSourceManager();
  CharSourceRange Range = CharSourceRange::getTokenRange(E->getSourceRange());
  return Lexer::getSourceText(Range, SM, C.getASTContext().getLangOpts()).trim();
}

static bool sameCleanupData(const Expr *ManualArg, const Expr *RegisteredArg,
                            CheckerContext &C) {
  const MemRegion *ManualRegion = getMemRegionFromExpr(ManualArg, C);
  const MemRegion *RegisteredRegion = getMemRegionFromExpr(RegisteredArg, C);
  if (ManualRegion && RegisteredRegion)
    return ManualRegion == RegisteredRegion;

  StringRef ManualText = getExprText(ManualArg, C);
  StringRef RegisteredText = getExprText(RegisteredArg, C);
  return !ManualText.empty() && ManualText == RegisteredText;
}

static bool evaluatesToZero(const Expr *E, CheckerContext &C) {
  llvm::APSInt Value;
  return E && EvaluateExprToInt(Value, E, C) && Value == 0;
}

static int failurePolarityForRegistrationCondition(const Expr *Cond,
                                                   const CallExpr *RegistrationCall,
                                                   CheckerContext &C) {
  if (!Cond || !RegistrationCall)
    return 0;

  Cond = Cond->IgnoreParenImpCasts();
  if (Cond == RegistrationCall)
    return 1;

  if (const auto *UO = dyn_cast<UnaryOperator>(Cond)) {
    if (UO->getOpcode() == UO_LNot)
      return -failurePolarityForRegistrationCondition(UO->getSubExpr(),
                                                       RegistrationCall, C);
  }

  if (const auto *BO = dyn_cast<BinaryOperator>(Cond)) {
    const Expr *LHS = BO->getLHS()->IgnoreParenImpCasts();
    const Expr *RHS = BO->getRHS()->IgnoreParenImpCasts();
    bool CallOnLHS = stmtContains(LHS, RegistrationCall);
    bool CallOnRHS = stmtContains(RHS, RegistrationCall);

    if (BO->getOpcode() == BO_LAnd || BO->getOpcode() == BO_LOr) {
      int LeftPolarity = CallOnLHS
                             ? failurePolarityForRegistrationCondition(
                                   LHS, RegistrationCall, C)
                             : 0;
      int RightPolarity = CallOnRHS
                              ? failurePolarityForRegistrationCondition(
                                    RHS, RegistrationCall, C)
                              : 0;
      if (LeftPolarity && RightPolarity && LeftPolarity != RightPolarity)
        return 0;
      return LeftPolarity ? LeftPolarity : RightPolarity;
    }

    if (CallOnLHS == CallOnRHS)
      return 0;

    const Expr *Other = CallOnLHS ? RHS : LHS;
    if (!evaluatesToZero(Other, C))
      return 0;

    switch (BO->getOpcode()) {
    case BO_EQ:
      return -1;
    case BO_NE:
      return 1;
    case BO_LT:
      return CallOnLHS ? 1 : -1;
    case BO_GT:
      return CallOnLHS ? -1 : 1;
    default:
      return 0;
    }
  }

  return stmtContains(Cond, RegistrationCall) ? 1 : 0;
}

class SAGenTestChecker : public Checker<check::PreCall> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Redundant devm cleanup", "Double Free")) {}

  void checkPreCall(const CallEvent &Call, CheckerContext &C) const;

private:
  void reportRedundantCleanup(const CallEvent &Call, CheckerContext &C) const;
};

void SAGenTestChecker::checkPreCall(const CallEvent &Call,
                                    CheckerContext &C) const {
  const Expr *OriginExpr = Call.getOriginExpr();
  if (!OriginExpr || Call.getNumArgs() < 1)
    return;

  const IfStmt *ParentIf = findSpecificTypeInParents<IfStmt>(OriginExpr, C);
  if (!ParentIf || !stmtContains(ParentIf->getThen(), OriginExpr))
    return;

  const CallExpr *RegistrationCall =
      findDevmAddActionOrResetCall(ParentIf->getCond());
  if (!RegistrationCall)
    return;

  if (failurePolarityForRegistrationCondition(ParentIf->getCond(),
                                              RegistrationCall, C) != 1)
    return;

  const FunctionDecl *RegisteredCleanup =
      getReferencedFunction(RegistrationCall->getArg(1));
  const FunctionDecl *ManualCleanup =
      canonicalFunction(dyn_cast_or_null<FunctionDecl>(Call.getDecl()));
  if (!RegisteredCleanup || RegisteredCleanup != ManualCleanup)
    return;

  if (!sameCleanupData(Call.getArgExpr(0), RegistrationCall->getArg(2), C))
    return;

  reportRedundantCleanup(Call, C);
}

void SAGenTestChecker::reportRedundantCleanup(const CallEvent &Call,
                                              CheckerContext &C) const {
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto Report = std::make_unique<PathSensitiveBugReport>(
      *BT,
      "devm_add_action_or_reset already invokes this cleanup on failure; "
      "calling it again can double free the registered data",
      N);
  Report->addRange(Call.getSourceRange());
  C.emitReport(std::move(Report));
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker", 
      "Detects redundant cleanup call in error handling leading to double free", "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
