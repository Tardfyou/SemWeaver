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

static bool isRiskyFieldName(StringRef Name) {
  return containsCaseInsensitive(Name, "dimension") ||
         containsCaseInsensitive(Name, "sampling") ||
         containsCaseInsensitive(Name, "subsampling") ||
         containsCaseInsensitive(Name, "channel") ||
         containsCaseInsensitive(Name, "depth") ||
         containsCaseInsensitive(Name, "plane") ||
         containsCaseInsensitive(Name, "component") ||
         containsCaseInsensitive(Name, "columns") ||
         containsCaseInsensitive(Name, "rows") ||
         containsCaseInsensitive(Name, "width") ||
         containsCaseInsensitive(Name, "height") ||
         containsCaseInsensitive(Name, "length") ||
         containsCaseInsensitive(Name, "size") ||
         containsCaseInsensitive(Name, "count");
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

static bool exprIsSensitiveUse(const Expr *E, const ValueDecl *Target) {
  E = E ? E->IgnoreParenCasts() : nullptr;
  if (!E || !Target)
    return false;

  if (!exprReferencesDecl(E, Target))
    return false;

  if (const auto *BO = dyn_cast<BinaryOperator>(E)) {
    BinaryOperatorKind Op = BO->getOpcode();
    return Op == BO_Mul || Op == BO_Div || Op == BO_Rem || Op == BO_Add ||
           Op == BO_Sub || Op == BO_Shl || Op == BO_Shr || Op == BO_LT ||
           Op == BO_LE || Op == BO_GT || Op == BO_GE || Op == BO_EQ ||
           Op == BO_NE || Op == BO_LAnd || Op == BO_LOr;
  }

  if (isa<ArraySubscriptExpr>(E) || isa<CallExpr>(E) || isa<ConditionalOperator>(E))
    return true;

  return false;
}

class MalformedHeaderFieldRangeChecker
    : public Checker<check::Bind, check::PreStmt<BinaryOperator>,
                     check::PreStmt<IfStmt>, check::PreCall> {
  mutable std::unique_ptr<BugType> BT;

public:
  MalformedHeaderFieldRangeChecker()
      : BT(std::make_unique<BugType>(this, "Malformed header field range",
                                     "Custom")) {}

  void checkBind(SVal Loc, SVal Val, const Stmt *StoreE, CheckerContext &C) const {
    const auto *DS = dyn_cast_or_null<DeclStmt>(StoreE);
    if (!DS || !DS->isSingleDecl())
      return;

    const auto *VD = dyn_cast<VarDecl>(DS->getSingleDecl());
    if (!VD || !VD->hasInit())
      return;

    const Expr *Init = VD->getInit()->IgnoreParenCasts();
    const auto *CE = dyn_cast<CallExpr>(Init);
    if (!CE)
      return;

    const FunctionDecl *FD = CE->getDirectCallee();
    if (!FD)
      return;
    const IdentifierInfo *II = FD->getIdentifier();
    if (!II)
      return;

    StringRef FuncName = II->getName();
    StringRef VarName = VD->getName();
    if (!isHeaderReadCallName(FuncName) || !isRiskyFieldName(VarName))
      return;

    const AnalysisDeclContext *ADC = C.getLocationContext()->getAnalysisDeclContext();
    if (!ADC)
      return;

    if (functionHasGuardForField(DS, VD, ADC))
      return;

    if (ExplodedNode *N = C.generateNonFatalErrorNode()) {
      auto R = std::make_unique<PathSensitiveBugReport>(
          *BT,
          "Header-derived field lacks semantic range validation after parsing; malformed input may reach later size, dimension, or indexing logic.",
          N);
      R->addRange(VD->getSourceRange());
      C.emitReport(std::move(R));
    }
  }

  void checkPreStmt(const BinaryOperator *BO, CheckerContext &C) const {
    if (!BO)
      return;

    const Expr *LHS = BO->getLHS();
    const Expr *RHS = BO->getRHS();
    const ValueDecl *Target = getBaseValueDecl(LHS);
    if (!Target)
      Target = getBaseValueDecl(RHS);
    if (!Target)
      return;

    StringRef Name = Target->getName();
    if (!isRiskyFieldName(Name))
      return;

    if (!exprIsSensitiveUse(BO, Target))
      return;

    const AnalysisDeclContext *ADC = C.getLocationContext()->getAnalysisDeclContext();
    if (!ADC)
      return;

    const Stmt *Body = ADC->getBody();
    const auto *CS = dyn_cast_or_null<CompoundStmt>(Body);
    if (!CS)
      return;

    bool HasGuard = false;
    for (const Stmt *Child : CS->body()) {
      if (const auto *IS = dyn_cast<IfStmt>(Child)) {
        if (!thenBranchLooksLikeBarrier(IS->getThen()))
          continue;
        if (conditionRejectsZeroOrLess(IS->getCond(), Target) &&
            conditionRejectsExcessiveValue(IS->getCond(), Target)) {
          HasGuard = true;
          break;
        }
      }
    }
    if (HasGuard)
      return;

    if (ExplodedNode *N = C.generateNonFatalErrorNode()) {
      auto R = std::make_unique<PathSensitiveBugReport>(
          *BT,
          "Sensitive arithmetic or control expression uses a header-derived field without a preceding semantic range check.",
          N);
      R->addRange(BO->getSourceRange());
      C.emitReport(std::move(R));
    }
  }

  void checkPreStmt(const IfStmt *IS, CheckerContext &C) const {
    if (!IS)
      return;

    const Expr *Cond = IS->getCond();
    if (!Cond)
      return;

    const ValueDecl *Target = getBaseValueDecl(Cond);
    if (!Target)
      return;

    StringRef Name = Target->getName();
    if (!isRiskyFieldName(Name))
      return;

    if (thenBranchLooksLikeBarrier(IS->getThen()))
      return;

    if (!exprIsSensitiveUse(Cond, Target))
      return;

    const AnalysisDeclContext *ADC = C.getLocationContext()->getAnalysisDeclContext();
    if (!ADC)
      return;

    const Stmt *Body = ADC->getBody();
    const auto *CS = dyn_cast_or_null<CompoundStmt>(Body);
    if (!CS)
      return;

    bool HasBarrierGuard = false;
    for (const Stmt *Child : CS->body()) {
      const auto *Guard = dyn_cast<IfStmt>(Child);
      if (!Guard)
        continue;
      if (!thenBranchLooksLikeBarrier(Guard->getThen()))
        continue;
      if (conditionRejectsZeroOrLess(Guard->getCond(), Target) &&
          conditionRejectsExcessiveValue(Guard->getCond(), Target)) {
        HasBarrierGuard = true;
        break;
      }
    }
    if (HasBarrierGuard)
      return;

    if (ExplodedNode *N = C.generateNonFatalErrorNode()) {
      auto R = std::make_unique<PathSensitiveBugReport>(
          *BT,
          "Control flow depends on a malformed-prone header field without an earlier rejecting range guard.",
          N);
      R->addRange(IS->getCond()->getSourceRange());
      C.emitReport(std::move(R));
    }
  }

  void checkPreCall(const CallEvent &Call, CheckerContext &C) const {
    const IdentifierInfo *II = Call.getCalleeIdentifier();
    if (!II)
      return;

    StringRef Callee = II->getName();
    if (!containsCaseInsensitive(Callee, "malloc") &&
        !containsCaseInsensitive(Callee, "alloc") &&
        !containsCaseInsensitive(Callee, "Acquire") &&
        !containsCaseInsensitive(Callee, "Set") &&
        !containsCaseInsensitive(Callee, "Queue") &&
        !containsCaseInsensitive(Callee, "Copy"))
      return;

    for (unsigned I = 0; I < Call.getNumArgs(); ++I) {
      const Expr *Arg = Call.getArgExpr(I);
      const ValueDecl *Target = getBaseValueDecl(Arg);
      if (!Target)
        continue;
      if (!isRiskyFieldName(Target->getName()))
        continue;
      if (!exprIsSensitiveUse(Arg, Target))
        continue;

      if (ExplodedNode *N = C.generateNonFatalErrorNode()) {
        auto R = std::make_unique<PathSensitiveBugReport>(
            *BT,
            "Header-derived field reaches allocation- or size-like API without clear semantic range validation.",
            N);
        R->addRange(Arg->getSourceRange());
        C.emitReport(std::move(R));
      }
      return;
    }
  }
};

} // namespace

extern "C" void clang_registerCheckers(CheckerRegistry &Registry) {
  Registry.addChecker<MalformedHeaderFieldRangeChecker>(
      "custom.MalformedHeaderFieldRangeChecker",
      "Detects header-derived fields used in sensitive size/dimension logic without semantic range guards.",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] = CLANG_VERSION_STRING;
