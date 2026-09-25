#include "clang/AST/Expr.h"
#include "clang/Basic/Version.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/CheckerManager.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/MemRegion.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramStateTrait.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SVals.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include <memory>

using namespace clang;
using namespace ento;

namespace {

static const Expr *ignoreParenCasts(const Expr *E) {
  return E ? E->IgnoreParenCasts() : nullptr;
}

static bool exprMentionsVar(const Expr *E, const VarDecl *VD) {
  E = ignoreParenCasts(E);
  if (!E || !VD)
    return false;

  if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
    return DRE->getDecl() == VD;

  for (const Stmt *Child : E->children()) {
    if (const auto *CE = dyn_cast_or_null<Expr>(Child)) {
      if (exprMentionsVar(CE, VD))
        return true;
    }
  }
  return false;
}

static bool isCheckedMulHelperName(StringRef Name) {
  return Name == "jas_safe_size_mul" || Name == "jas_safe_int_mul" ||
         Name == "jas_safe_uint_mul" || Name == "jas_safe_ulong_mul" ||
         Name == "jas_safe_ulonglong_mul" || Name == "jas_safe_long_mul" ||
         Name == "__builtin_mul_overflow" || Name.ends_with("_safe_mul") ||
         Name.contains("mul_overflow");
}

static bool isAllocationLikeName(StringRef Name) {
  return Name == "malloc" || Name == "calloc" || Name == "realloc" ||
         Name == "jas_alloc2" || Name == "jas_alloc3" ||
         Name.ends_with("alloc") || Name.ends_with("alloc2") ||
         Name.ends_with("alloc3") || Name.contains("alloc");
}

static const VarDecl *getAssignedVarFromExpr(const Expr *E) {
  E = ignoreParenCasts(E);
  if (!E)
    return nullptr;
  if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
    return dyn_cast<VarDecl>(DRE->getDecl());
  return nullptr;
}

class IntegerOverflowChecker
    : public Checker<check::PreCall, check::PreStmt<BinaryOperator>> {
  mutable std::unique_ptr<BugType> BT;

public:
  IntegerOverflowChecker()
      : BT(std::make_unique<BugType>(this,
                                     "Unchecked multiplication used as count",
                                     "Integer Overflow")) {}

  void checkPreStmt(const BinaryOperator *BO, CheckerContext &C) const {
    if (!BO || !BO->isAssignmentOp())
      return;

    const Expr *RHS = ignoreParenCasts(BO->getRHS());
    const auto *Mul = dyn_cast_or_null<BinaryOperator>(RHS);
    if (!Mul || Mul->getOpcode() != BO_Mul)
      return;

    const VarDecl *AssignedVD = getAssignedVarFromExpr(BO->getLHS());
    if (!AssignedVD)
      return;

    const Stmt *Parent = C.getCurrentAnalysisDeclContext()->getParentMap().getParent(BO);
    if (!Parent)
      return;

    const auto *CS = dyn_cast<CompoundStmt>(Parent);
    if (!CS)
      return;

    bool SeenThisStmt = false;
    bool GuardedBeforeUse = false;

    for (const Stmt *S : CS->body()) {
      if (S == BO) {
        SeenThisStmt = true;
        continue;
      }
      if (!SeenThisStmt)
        continue;

      if (const auto *InnerIf = dyn_cast<IfStmt>(S)) {
        const Expr *Cond = ignoreParenCasts(InnerIf->getCond());
        if (const auto *Call = dyn_cast_or_null<CallExpr>(Cond)) {
          const FunctionDecl *FD = Call->getDirectCallee();
          if (FD && isCheckedMulHelperName(FD->getName())) {
            if (Call->getNumArgs() >= 3) {
              const Expr *Arg0 = Call->getArg(0);
              const Expr *Arg1 = Call->getArg(1);
              if ((exprMentionsVar(Arg0, AssignedVD) || exprMentionsVar(Arg1, AssignedVD)) ||
                  (exprMentionsVar(Arg0, getAssignedVarFromExpr(Mul->getLHS())) &&
                   exprMentionsVar(Arg1, getAssignedVarFromExpr(Mul->getRHS())))) {
                GuardedBeforeUse = true;
                break;
              }
            }
          }
        } else if (exprMentionsVar(Cond, AssignedVD)) {
          GuardedBeforeUse = true;
          break;
        }
      }

      if (const auto *Call = dyn_cast<CallExpr>(S)) {
        const FunctionDecl *FD = Call->getDirectCallee();
        if (!FD)
          continue;
        if (!isAllocationLikeName(FD->getName()))
          continue;

        bool UsesAssignedVar = false;
        for (unsigned I = 0; I < Call->getNumArgs(); ++I) {
          if (exprMentionsVar(Call->getArg(I), AssignedVD)) {
            UsesAssignedVar = true;
            break;
          }
        }
        if (!UsesAssignedVar)
          continue;
        if (GuardedBeforeUse)
          return;

        ExplodedNode *N = C.generateNonFatalErrorNode();
        if (!N)
          return;

        auto R = std::make_unique<PathSensitiveBugReport>(
            *BT,
            "Result of unchecked multiplication is stored in a count variable and then used by an allocation-like call; this can overflow and corrupt allocation cardinality.",
            N);
        R->addRange(Mul->getSourceRange());
        C.emitReport(std::move(R));
        return;
      }
    }
  }

  void checkPreCall(const CallEvent &Call, CheckerContext &C) const {
    const IdentifierInfo *II = Call.getCalleeIdentifier();
    if (!II)
      return;

    StringRef Callee = II->getName();
    if (!isAllocationLikeName(Callee))
      return;

    for (unsigned I = 0; I < Call.getNumArgs(); ++I) {
      const Expr *Arg = Call.getArgExpr(I);
      Arg = ignoreParenCasts(Arg);
      const auto *Mul = dyn_cast_or_null<BinaryOperator>(Arg);
      if (!Mul || Mul->getOpcode() != BO_Mul)
        continue;

      ExplodedNode *N = C.generateNonFatalErrorNode();
      if (!N)
        return;

      auto R = std::make_unique<PathSensitiveBugReport>(
          *BT,
          "Unchecked multiplication flows directly into an allocation-like call as a count/size argument and may overflow.",
          N);
      R->addRange(Mul->getSourceRange());
      C.emitReport(std::move(R));
      return;
    }
  }
};

} // namespace

extern "C" void clang_registerCheckers(CheckerRegistry &Registry) {
  Registry.addChecker<IntegerOverflowChecker>(
      "custom.IntegerOverflowChecker",
      "Detect unchecked multiplication used as allocation count or size.",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] = CLANG_VERSION_STRING;
