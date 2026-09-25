#include "clang/AST/Expr.h"
#include "clang/Basic/Version.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/CheckerManager.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SVals.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include <memory>

using namespace clang;
using namespace ento;

namespace {

static bool containsCaseInsensitive(StringRef Haystack, StringRef Needle) {
  return Haystack.contains_insensitive(Needle);
}

static bool isZeroIntegerLiteral(const Expr *E) {
  E = E ? E->IgnoreParenCasts() : nullptr;
  if (const auto *IL = dyn_cast_or_null<IntegerLiteral>(E))
    return IL->getValue().isZero();
  return false;
}

static bool isOneIntegerLiteral(const Expr *E) {
  E = E ? E->IgnoreParenCasts() : nullptr;
  if (const auto *IL = dyn_cast_or_null<IntegerLiteral>(E))
    return IL->getValue() == 1;
  return false;
}

static const ValueDecl *getBaseValueDecl(const Expr *E) {
  E = E ? E->IgnoreParenCasts() : nullptr;
  if (!E)
    return nullptr;
  if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
    return DRE->getDecl();
  if (const auto *ME = dyn_cast<MemberExpr>(E))
    return ME->getMemberDecl();
  if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(E))
    return getBaseValueDecl(ASE->getBase());
  if (const auto *UO = dyn_cast<UnaryOperator>(E))
    return getBaseValueDecl(UO->getSubExpr());
  if (const auto *BO = dyn_cast<BinaryOperator>(E)) {
    if (const ValueDecl *VD = getBaseValueDecl(BO->getLHS()))
      return VD;
    return getBaseValueDecl(BO->getRHS());
  }
  if (const auto *CO = dyn_cast<ConditionalOperator>(E)) {
    if (const ValueDecl *VD = getBaseValueDecl(CO->getTrueExpr()))
      return VD;
    return getBaseValueDecl(CO->getFalseExpr());
  }
  return nullptr;
}

static bool isDimensionLikeFieldName(StringRef Name) {
  return containsCaseInsensitive(Name, "dimension") ||
         containsCaseInsensitive(Name, "depth") ||
         containsCaseInsensitive(Name, "channel") ||
         containsCaseInsensitive(Name, "component") ||
         containsCaseInsensitive(Name, "plane") ||
         containsCaseInsensitive(Name, "sampling") ||
         containsCaseInsensitive(Name, "subsampling");
}

static bool isHeaderReadCallName(StringRef Name) {
  return containsCaseInsensitive(Name, "ReadBlob") ||
         containsCaseInsensitive(Name, "ReadHeader") ||
         containsCaseInsensitive(Name, "ReadShort") ||
         containsCaseInsensitive(Name, "ReadLong") ||
         containsCaseInsensitive(Name, "read_u") ||
         containsCaseInsensitive(Name, "read16") ||
         containsCaseInsensitive(Name, "read32");
}

static bool isBarrierLikeCallName(StringRef Name) {
  return containsCaseInsensitive(Name, "Throw") ||
         containsCaseInsensitive(Name, "Error") ||
         containsCaseInsensitive(Name, "Exception") ||
         containsCaseInsensitive(Name, "Fatal") ||
         containsCaseInsensitive(Name, "abort") ||
         containsCaseInsensitive(Name, "exit");
}

static bool exprReferencesDecl(const Expr *E, const ValueDecl *Target) {
  E = E ? E->IgnoreParenCasts() : nullptr;
  if (!E || !Target)
    return false;

  if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
    return DRE->getDecl() == Target;
  if (const auto *ME = dyn_cast<MemberExpr>(E))
    return ME->getMemberDecl() == Target;
  if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(E))
    return exprReferencesDecl(ASE->getBase(), Target) ||
           exprReferencesDecl(ASE->getIdx(), Target);
  if (const auto *UO = dyn_cast<UnaryOperator>(E))
    return exprReferencesDecl(UO->getSubExpr(), Target);
  if (const auto *BO = dyn_cast<BinaryOperator>(E))
    return exprReferencesDecl(BO->getLHS(), Target) ||
           exprReferencesDecl(BO->getRHS(), Target);
  if (const auto *CO = dyn_cast<ConditionalOperator>(E))
    return exprReferencesDecl(CO->getCond(), Target) ||
           exprReferencesDecl(CO->getTrueExpr(), Target) ||
           exprReferencesDecl(CO->getFalseExpr(), Target);
  if (const auto *CE = dyn_cast<CallExpr>(E)) {
    for (const Expr *Arg : CE->arguments()) {
      if (exprReferencesDecl(Arg, Target))
        return true;
    }
    return false;
  }
  return false;
}

static bool conditionRejectsZeroOrLess(const Expr *Cond, const ValueDecl *Target) {
  Cond = Cond ? Cond->IgnoreParenCasts() : nullptr;
  if (!Cond || !Target)
    return false;

  if (const auto *BO = dyn_cast<BinaryOperator>(Cond)) {
    if (BO->getOpcode() == BO_LAnd || BO->getOpcode() == BO_LOr)
      return conditionRejectsZeroOrLess(BO->getLHS(), Target) ||
             conditionRejectsZeroOrLess(BO->getRHS(), Target);

    const Expr *LHS = BO->getLHS()->IgnoreParenCasts();
    const Expr *RHS = BO->getRHS()->IgnoreParenCasts();

    if (exprReferencesDecl(LHS, Target)) {
      switch (BO->getOpcode()) {
      case BO_EQ:
        return isZeroIntegerLiteral(RHS);
      case BO_LE:
        return isZeroIntegerLiteral(RHS);
      case BO_LT:
        return isOneIntegerLiteral(RHS);
      default:
        break;
      }
    }
    if (exprReferencesDecl(RHS, Target)) {
      switch (BO->getOpcode()) {
      case BO_EQ:
        return isZeroIntegerLiteral(LHS);
      case BO_GE:
        return isZeroIntegerLiteral(LHS);
      case BO_GT:
        return isOneIntegerLiteral(LHS);
      default:
        break;
      }
    }
    return false;
  }

  if (const auto *UO = dyn_cast<UnaryOperator>(Cond)) {
    if (UO->getOpcode() == UO_LNot)
      return exprReferencesDecl(UO->getSubExpr(), Target);
  }

  return false;
}

static bool conditionRejectsExcessiveValue(const Expr *Cond, const ValueDecl *Target) {
  Cond = Cond ? Cond->IgnoreParenCasts() : nullptr;
  if (!Cond || !Target)
    return false;

  if (const auto *BO = dyn_cast<BinaryOperator>(Cond)) {
    if (BO->getOpcode() == BO_LAnd || BO->getOpcode() == BO_LOr)
      return conditionRejectsExcessiveValue(BO->getLHS(), Target) ||
             conditionRejectsExcessiveValue(BO->getRHS(), Target);

    const Expr *LHS = BO->getLHS()->IgnoreParenCasts();
    const Expr *RHS = BO->getRHS()->IgnoreParenCasts();

    if (exprReferencesDecl(LHS, Target)) {
      switch (BO->getOpcode()) {
      case BO_GT:
      case BO_GE:
        return isa<IntegerLiteral>(RHS->IgnoreParenCasts());
      default:
        break;
      }
    }
    if (exprReferencesDecl(RHS, Target)) {
      switch (BO->getOpcode()) {
      case BO_LT:
      case BO_LE:
        return isa<IntegerLiteral>(LHS->IgnoreParenCasts());
      default:
        break;
      }
    }
    return false;
  }

  return false;
}

static bool thenBranchLooksLikeBarrier(const Stmt *S) {
  if (!S)
    return false;

  if (const auto *CS = dyn_cast<CompoundStmt>(S)) {
    for (const Stmt *Child : CS->body()) {
      if (!Child)
        continue;
      if (isa<ReturnStmt>(Child))
        return true;
      if (const auto *CE = dyn_cast<CallExpr>(Child)) {
        if (const FunctionDecl *FD = CE->getDirectCallee()) {
          if (const IdentifierInfo *II = FD->getIdentifier()) {
            if (isBarrierLikeCallName(II->getName()))
              return true;
          }
        }
      }
    }
    return false;
  }

  if (isa<ReturnStmt>(S))
    return true;

  if (const auto *CE = dyn_cast<CallExpr>(S)) {
    if (const FunctionDecl *FD = CE->getDirectCallee()) {
      if (const IdentifierInfo *II = FD->getIdentifier())
        return isBarrierLikeCallName(II->getName());
    }
  }

  return false;
}

static bool functionHasGuardForField(const DeclStmt *ReadDS, const ValueDecl *Target,
                                     const AnalysisDeclContext *ADC) {
  if (!ReadDS || !Target || !ADC)
    return false;

  const Stmt *Body = ADC->getBody();
  const auto *CS = dyn_cast_or_null<CompoundStmt>(Body);
  if (!CS)
    return false;

  bool SeenRead = false;
  bool HasZeroLikeGuard = false;
  bool HasUpperGuard = false;

  for (const Stmt *Child : CS->body()) {
    if (!Child)
      continue;
    if (Child == ReadDS) {
      SeenRead = true;
      continue;
    }
    if (!SeenRead)
      continue;

    const auto *IS = dyn_cast<IfStmt>(Child);
    if (!IS)
      continue;
    if (!thenBranchLooksLikeBarrier(IS->getThen()))
      continue;

    const Expr *Cond = IS->getCond();
    if (!HasZeroLikeGuard && conditionRejectsZeroOrLess(Cond, Target))
      HasZeroLikeGuard = true;
    if (!HasUpperGuard && conditionRejectsExcessiveValue(Cond, Target))
      HasUpperGuard = true;

    if (HasZeroLikeGuard && HasUpperGuard)
      return true;
  }

  return false;
}

static bool isDimensionReadDecl(const DeclStmt *DS) {
  if (!DS || !DS->isSingleDecl())
    return false;

  const auto *VD = dyn_cast<VarDecl>(DS->getSingleDecl());
  if (!VD || !VD->hasInit())
    return false;

  const auto *CE = dyn_cast<CallExpr>(VD->getInit()->IgnoreParenCasts());
  if (!CE)
    return false;

  const FunctionDecl *FD = CE->getDirectCallee();
  const IdentifierInfo *II = FD ? FD->getIdentifier() : nullptr;
  if (!II)
    return false;

  return isHeaderReadCallName(II->getName()) &&
         isDimensionLikeFieldName(VD->getName());
}

static bool stmtUsesDecl(const Stmt *S, const ValueDecl *Target) {
  if (!S || !Target)
    return false;

  if (const auto *E = dyn_cast<Expr>(S))
    return exprReferencesDecl(E, Target);

  for (const Stmt *Child : S->children()) {
    if (stmtUsesDecl(Child, Target))
      return true;
  }
  return false;
}

class MalformedHeaderFieldRangeChecker
    : public Checker<check::Bind> {
  mutable std::unique_ptr<BugType> BT;

public:
  MalformedHeaderFieldRangeChecker()
      : BT(std::make_unique<BugType>(this, "Malformed header field range",
                                     "Custom")) {}

  void checkBind(SVal Loc, SVal Val, const Stmt *StoreE, CheckerContext &C) const {
    const auto *DS = dyn_cast_or_null<DeclStmt>(StoreE);
    if (!isDimensionReadDecl(DS))
      return;

    const auto *VD = dyn_cast<VarDecl>(DS->getSingleDecl());
    const AnalysisDeclContext *ADC = C.getLocationContext()->getAnalysisDeclContext();
    if (!VD || !ADC)
      return;

    const auto *CS = dyn_cast_or_null<CompoundStmt>(ADC->getBody());
    if (!CS)
      return;

    bool SeenRead = false;
    bool HasZeroLikeGuard = false;
    bool HasUpperGuard = false;
    bool HasLaterUse = false;

    for (const Stmt *Child : CS->body()) {
      if (!Child)
        continue;
      if (Child == DS) {
        SeenRead = true;
        continue;
      }
      if (!SeenRead)
        continue;

      if (const auto *IS = dyn_cast<IfStmt>(Child)) {
        if (thenBranchLooksLikeBarrier(IS->getThen())) {
          const Expr *Cond = IS->getCond();
          if (!HasZeroLikeGuard && conditionRejectsZeroOrLess(Cond, VD))
            HasZeroLikeGuard = true;
          if (!HasUpperGuard && conditionRejectsExcessiveValue(Cond, VD))
            HasUpperGuard = true;
          continue;
        }
      }

      if (stmtUsesDecl(Child, VD))
        HasLaterUse = true;
    }

    if (!HasLaterUse)
      return;
    if (HasZeroLikeGuard && HasUpperGuard)
      return;

    if (ExplodedNode *N = C.generateNonFatalErrorNode()) {
      auto R = std::make_unique<PathSensitiveBugReport>(
          *BT,
          "Header-derived dimension-like field is used after parsing without both rejecting zero and bounding the upper range via a barrier guard.",
          N);
      R->addRange(VD->getSourceRange());
      C.emitReport(std::move(R));
    }
  }
};

} // namespace

extern "C" void clang_registerCheckers(CheckerRegistry &Registry) {
  Registry.addChecker<MalformedHeaderFieldRangeChecker>(
      "custom.MalformedHeaderFieldRangeChecker",
      "Detects dimension-like header fields that are parsed and later used without a rejecting lower-bound and upper-bound barrier guard.",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] = CLANG_VERSION_STRING;
