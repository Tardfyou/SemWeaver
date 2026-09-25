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
    return getBaseValueDecl(ME->getBase());
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

static const FieldDecl *getReferencedField(const Expr *E) {
  E = E ? E->IgnoreParenCasts() : nullptr;
  if (!E)
    return nullptr;
  if (const auto *ME = dyn_cast<MemberExpr>(E))
    return dyn_cast<FieldDecl>(ME->getMemberDecl());
  if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(E))
    return getReferencedField(ASE->getBase());
  if (const auto *UO = dyn_cast<UnaryOperator>(E))
    return getReferencedField(UO->getSubExpr());
  if (const auto *BO = dyn_cast<BinaryOperator>(E)) {
    if (const FieldDecl *FD = getReferencedField(BO->getLHS()))
      return FD;
    return getReferencedField(BO->getRHS());
  }
  if (const auto *CO = dyn_cast<ConditionalOperator>(E)) {
    if (const FieldDecl *FD = getReferencedField(CO->getTrueExpr()))
      return FD;
    return getReferencedField(CO->getFalseExpr());
  }
  return nullptr;
}

static bool exprReferencesFieldOnBase(const Expr *E, const FieldDecl *TargetField,
                                      const ValueDecl *BaseDecl) {
  E = E ? E->IgnoreParenCasts() : nullptr;
  if (!E || !TargetField || !BaseDecl)
    return false;

  if (const auto *ME = dyn_cast<MemberExpr>(E)) {
    return ME->getMemberDecl() == TargetField &&
           getBaseValueDecl(ME->getBase()) == BaseDecl;
  }
  if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(E))
    return exprReferencesFieldOnBase(ASE->getBase(), TargetField, BaseDecl) ||
           exprReferencesFieldOnBase(ASE->getIdx(), TargetField, BaseDecl);
  if (const auto *UO = dyn_cast<UnaryOperator>(E))
    return exprReferencesFieldOnBase(UO->getSubExpr(), TargetField, BaseDecl);
  if (const auto *BO = dyn_cast<BinaryOperator>(E))
    return exprReferencesFieldOnBase(BO->getLHS(), TargetField, BaseDecl) ||
           exprReferencesFieldOnBase(BO->getRHS(), TargetField, BaseDecl);
  if (const auto *CO = dyn_cast<ConditionalOperator>(E))
    return exprReferencesFieldOnBase(CO->getCond(), TargetField, BaseDecl) ||
           exprReferencesFieldOnBase(CO->getTrueExpr(), TargetField, BaseDecl) ||
           exprReferencesFieldOnBase(CO->getFalseExpr(), TargetField, BaseDecl);
  if (const auto *CE = dyn_cast<CallExpr>(E)) {
    for (const Expr *Arg : CE->arguments()) {
      if (exprReferencesFieldOnBase(Arg, TargetField, BaseDecl))
        return true;
    }
  }
  return false;
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

static bool isCountLikeFieldName(StringRef Name) {
  return isDimensionLikeFieldName(Name) ||
         containsCaseInsensitive(Name, "count") ||
         containsCaseInsensitive(Name, "number") ||
         containsCaseInsensitive(Name, "size") ||
         containsCaseInsensitive(Name, "length") ||
         containsCaseInsensitive(Name, "width") ||
         containsCaseInsensitive(Name, "height") ||
         containsCaseInsensitive(Name, "column") ||
         containsCaseInsensitive(Name, "row");
}

static bool isImageStructureLikeName(StringRef Name) {
  return containsCaseInsensitive(Name, "iris") ||
         containsCaseInsensitive(Name, "header") ||
         containsCaseInsensitive(Name, "image") ||
         containsCaseInsensitive(Name, "info");
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
    return ME->getMemberDecl() == Target ||
           exprReferencesDecl(ME->getBase(), Target);
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

static bool conditionRejectsZeroOrLess(const Expr *Cond, const ValueDecl *Target,
                                       const FieldDecl *TargetField,
                                       const ValueDecl *BaseDecl) {
  Cond = Cond ? Cond->IgnoreParenCasts() : nullptr;
  if (!Cond)
    return false;

  auto referencesTarget = [&](const Expr *E) {
    return (Target && exprReferencesDecl(E, Target)) ||
           (TargetField && BaseDecl &&
            exprReferencesFieldOnBase(E, TargetField, BaseDecl));
  };

  if (const auto *BO = dyn_cast<BinaryOperator>(Cond)) {
    if (BO->getOpcode() == BO_LAnd || BO->getOpcode() == BO_LOr)
      return conditionRejectsZeroOrLess(BO->getLHS(), Target, TargetField, BaseDecl) ||
             conditionRejectsZeroOrLess(BO->getRHS(), Target, TargetField, BaseDecl);

    const Expr *LHS = BO->getLHS()->IgnoreParenCasts();
    const Expr *RHS = BO->getRHS()->IgnoreParenCasts();

    if (referencesTarget(LHS)) {
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
    if (referencesTarget(RHS)) {
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
      return referencesTarget(UO->getSubExpr());
  }

  return false;
}

static bool conditionRejectsExcessiveValue(const Expr *Cond, const ValueDecl *Target,
                                           const FieldDecl *TargetField,
                                           const ValueDecl *BaseDecl) {
  Cond = Cond ? Cond->IgnoreParenCasts() : nullptr;
  if (!Cond)
    return false;

  auto referencesTarget = [&](const Expr *E) {
    return (Target && exprReferencesDecl(E, Target)) ||
           (TargetField && BaseDecl &&
            exprReferencesFieldOnBase(E, TargetField, BaseDecl));
  };

  if (const auto *BO = dyn_cast<BinaryOperator>(Cond)) {
    if (BO->getOpcode() == BO_LAnd || BO->getOpcode() == BO_LOr)
      return conditionRejectsExcessiveValue(BO->getLHS(), Target, TargetField, BaseDecl) ||
             conditionRejectsExcessiveValue(BO->getRHS(), Target, TargetField, BaseDecl);

    const Expr *LHS = BO->getLHS()->IgnoreParenCasts();
    const Expr *RHS = BO->getRHS()->IgnoreParenCasts();

    if (referencesTarget(LHS)) {
      switch (BO->getOpcode()) {
      case BO_GT:
      case BO_GE:
        return isa<IntegerLiteral>(RHS->IgnoreParenCasts());
      default:
        break;
      }
    }
    if (referencesTarget(RHS)) {
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

static bool isDimensionFieldReadAssignment(const Stmt *S, const ValueDecl *&BaseDecl,
                                           const FieldDecl *&Field) {
  BaseDecl = nullptr;
  Field = nullptr;

  const auto *BO = dyn_cast_or_null<BinaryOperator>(S);
  if (!BO || !BO->isAssignmentOp())
    return false;

  const auto *LHS = dyn_cast<MemberExpr>(BO->getLHS()->IgnoreParenCasts());
  if (!LHS)
    return false;

  const auto *FD = dyn_cast<FieldDecl>(LHS->getMemberDecl());
  if (!FD || !isDimensionLikeFieldName(FD->getName()))
    return false;

  const ValueDecl *Owner = getBaseValueDecl(LHS->getBase());
  if (!Owner || !isImageStructureLikeName(Owner->getName()))
    return false;

  const auto *RHS = dyn_cast<CallExpr>(BO->getRHS()->IgnoreParenCasts());
  if (!RHS)
    return false;

  const FunctionDecl *Callee = RHS->getDirectCallee();
  const IdentifierInfo *II = Callee ? Callee->getIdentifier() : nullptr;
  if (!II || !isHeaderReadCallName(II->getName()))
    return false;

  BaseDecl = Owner;
  Field = FD;
  return true;
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

static bool exprUsesValueAsIndexOrCount(const Expr *E, const ValueDecl *Target) {
  E = E ? E->IgnoreParenCasts() : nullptr;
  if (!E || !Target)
    return false;

  if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(E))
    return exprReferencesDecl(ASE->getIdx(), Target) ||
           exprUsesValueAsIndexOrCount(ASE->getBase(), Target) ||
           exprUsesValueAsIndexOrCount(ASE->getIdx(), Target);

  if (const auto *BO = dyn_cast<BinaryOperator>(E)) {
    if (BO->isMultiplicativeOp() || BO->isAdditiveOp() || BO->isShiftOp())
      return exprReferencesDecl(BO->getLHS(), Target) ||
             exprReferencesDecl(BO->getRHS(), Target) ||
             exprUsesValueAsIndexOrCount(BO->getLHS(), Target) ||
             exprUsesValueAsIndexOrCount(BO->getRHS(), Target);
    return exprUsesValueAsIndexOrCount(BO->getLHS(), Target) ||
           exprUsesValueAsIndexOrCount(BO->getRHS(), Target);
  }

  if (const auto *UO = dyn_cast<UnaryOperator>(E))
    return exprUsesValueAsIndexOrCount(UO->getSubExpr(), Target);

  if (const auto *CO = dyn_cast<ConditionalOperator>(E))
    return exprUsesValueAsIndexOrCount(CO->getCond(), Target) ||
           exprUsesValueAsIndexOrCount(CO->getTrueExpr(), Target) ||
           exprUsesValueAsIndexOrCount(CO->getFalseExpr(), Target);

  if (const auto *CE = dyn_cast<CallExpr>(E)) {
    for (const Expr *Arg : CE->arguments()) {
      if (exprReferencesDecl(Arg, Target) || exprUsesValueAsIndexOrCount(Arg, Target))
        return true;
    }
  }

  return false;
}

static bool exprUsesFieldAsIndexOrCount(const Expr *E, const FieldDecl *TargetField,
                                        const ValueDecl *BaseDecl) {
  E = E ? E->IgnoreParenCasts() : nullptr;
  if (!E || !TargetField || !BaseDecl)
    return false;

  if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(E))
    return exprReferencesFieldOnBase(ASE->getIdx(), TargetField, BaseDecl) ||
           exprUsesFieldAsIndexOrCount(ASE->getBase(), TargetField, BaseDecl) ||
           exprUsesFieldAsIndexOrCount(ASE->getIdx(), TargetField, BaseDecl);

  if (const auto *BO = dyn_cast<BinaryOperator>(E)) {
    if (BO->isMultiplicativeOp() || BO->isAdditiveOp() || BO->isShiftOp())
      return exprReferencesFieldOnBase(BO->getLHS(), TargetField, BaseDecl) ||
             exprReferencesFieldOnBase(BO->getRHS(), TargetField, BaseDecl) ||
             exprUsesFieldAsIndexOrCount(BO->getLHS(), TargetField, BaseDecl) ||
             exprUsesFieldAsIndexOrCount(BO->getRHS(), TargetField, BaseDecl);
    return exprUsesFieldAsIndexOrCount(BO->getLHS(), TargetField, BaseDecl) ||
           exprUsesFieldAsIndexOrCount(BO->getRHS(), TargetField, BaseDecl);
  }

  if (const auto *UO = dyn_cast<UnaryOperator>(E))
    return exprUsesFieldAsIndexOrCount(UO->getSubExpr(), TargetField, BaseDecl);

  if (const auto *CO = dyn_cast<ConditionalOperator>(E))
    return exprUsesFieldAsIndexOrCount(CO->getCond(), TargetField, BaseDecl) ||
           exprUsesFieldAsIndexOrCount(CO->getTrueExpr(), TargetField, BaseDecl) ||
           exprUsesFieldAsIndexOrCount(CO->getFalseExpr(), TargetField, BaseDecl);

  if (const auto *CE = dyn_cast<CallExpr>(E)) {
    for (const Expr *Arg : CE->arguments()) {
      if (exprReferencesFieldOnBase(Arg, TargetField, BaseDecl) ||
          exprUsesFieldAsIndexOrCount(Arg, TargetField, BaseDecl))
        return true;
    }
  }

  return false;
}

static bool stmtUsesDeclAsIndexOrCount(const Stmt *S, const ValueDecl *Target) {
  if (!S || !Target)
    return false;

  if (const auto *E = dyn_cast<Expr>(S))
    return exprUsesValueAsIndexOrCount(E, Target);

  for (const Stmt *Child : S->children()) {
    if (stmtUsesDeclAsIndexOrCount(Child, Target))
      return true;
  }
  return false;
}

static bool stmtUsesFieldAsIndexOrCount(const Stmt *S, const FieldDecl *TargetField,
                                        const ValueDecl *BaseDecl) {
  if (!S || !TargetField || !BaseDecl)
    return false;

  if (const auto *E = dyn_cast<Expr>(S))
    return exprUsesFieldAsIndexOrCount(E, TargetField, BaseDecl);

  for (const Stmt *Child : S->children()) {
    if (stmtUsesFieldAsIndexOrCount(Child, TargetField, BaseDecl))
      return true;
  }
  return false;
}

static bool stmtWritesFieldWithDecl(const Stmt *S, const ValueDecl *Target) {
  const auto *BO = dyn_cast_or_null<BinaryOperator>(S);
  if (!BO || !BO->isAssignmentOp() || !Target)
    return false;

  const auto *LHS = dyn_cast<MemberExpr>(BO->getLHS()->IgnoreParenCasts());
  if (!LHS)
    return false;

  const auto *FD = dyn_cast<FieldDecl>(LHS->getMemberDecl());
  if (!FD || !isCountLikeFieldName(FD->getName()))
    return false;

  return exprReferencesDecl(BO->getRHS(), Target);
}

static bool stmtWritesRelatedFieldWithField(const Stmt *S, const FieldDecl *TargetField,
                                            const ValueDecl *BaseDecl) {
  const auto *BO = dyn_cast_or_null<BinaryOperator>(S);
  if (!BO || !BO->isAssignmentOp() || !TargetField || !BaseDecl)
    return false;

  const auto *LHS = dyn_cast<MemberExpr>(BO->getLHS()->IgnoreParenCasts());
  if (!LHS)
    return false;

  const auto *FD = dyn_cast<FieldDecl>(LHS->getMemberDecl());
  if (!FD || !isCountLikeFieldName(FD->getName()))
    return false;

  const ValueDecl *Owner = getBaseValueDecl(LHS->getBase());
  if (Owner != BaseDecl)
    return false;

  return exprReferencesFieldOnBase(BO->getRHS(), TargetField, BaseDecl);
}

class MalformedHeaderFieldRangeChecker
    : public Checker<check::Bind> {
  mutable std::unique_ptr<BugType> BT;

public:
  MalformedHeaderFieldRangeChecker()
      : BT(std::make_unique<BugType>(this, "Malformed header field range",
                                     "Custom")) {}

  void checkBind(SVal Loc, SVal Val, const Stmt *StoreE, CheckerContext &C) const {
    const AnalysisDeclContext *ADC = C.getLocationContext()->getAnalysisDeclContext();
    if (!ADC)
      return;

    const auto *CS = dyn_cast_or_null<CompoundStmt>(ADC->getBody());
    if (!CS)
      return;

    const auto *DS = dyn_cast_or_null<DeclStmt>(StoreE);
    if (isDimensionReadDecl(DS)) {
      const auto *VD = dyn_cast<VarDecl>(DS->getSingleDecl());
      if (!VD)
        return;

      bool SeenRead = false;
      bool HasZeroLikeGuard = false;
      bool HasUpperGuard = false;
      bool HasLaterDangerousUse = false;

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
            if (!HasZeroLikeGuard &&
                conditionRejectsZeroOrLess(Cond, VD, nullptr, nullptr))
              HasZeroLikeGuard = true;
            if (!HasUpperGuard &&
                conditionRejectsExcessiveValue(Cond, VD, nullptr, nullptr))
              HasUpperGuard = true;
            continue;
          }
        }

        if (stmtUsesDeclAsIndexOrCount(Child, VD) ||
            stmtWritesFieldWithDecl(Child, VD))
          HasLaterDangerousUse = true;
      }

      if (!HasLaterDangerousUse)
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
      return;
    }

    const ValueDecl *BaseDecl = nullptr;
    const FieldDecl *Field = nullptr;
    if (!isDimensionFieldReadAssignment(StoreE, BaseDecl, Field))
      return;

    bool SeenRead = false;
    bool HasZeroLikeGuard = false;
    bool HasUpperGuard = false;
    bool HasLaterDangerousFieldUse = false;

    for (const Stmt *Child : CS->body()) {
      if (!Child)
        continue;
      if (Child == StoreE) {
        SeenRead = true;
        continue;
      }
      if (!SeenRead)
        continue;

      if (const auto *IS = dyn_cast<IfStmt>(Child)) {
        if (thenBranchLooksLikeBarrier(IS->getThen())) {
          const Expr *Cond = IS->getCond();
          if (!HasZeroLikeGuard &&
              conditionRejectsZeroOrLess(Cond, nullptr, Field, BaseDecl))
            HasZeroLikeGuard = true;
          if (!HasUpperGuard &&
              conditionRejectsExcessiveValue(Cond, nullptr, Field, BaseDecl))
            HasUpperGuard = true;
          continue;
        }
      }

      if (stmtUsesFieldAsIndexOrCount(Child, Field, BaseDecl) ||
          stmtWritesRelatedFieldWithField(Child, Field, BaseDecl))
        HasLaterDangerousFieldUse = true;
    }

    if (!HasLaterDangerousFieldUse)
      return;
    if (HasZeroLikeGuard && HasUpperGuard)
      return;

    if (ExplodedNode *N = C.generateNonFatalErrorNode()) {
      auto R = std::make_unique<PathSensitiveBugReport>(
          *BT,
          "Header-derived dimension field stored into an image-like header object is later used without both rejecting zero and bounding the upper range via a barrier guard.",
          N);
      R->addRange(Field->getSourceRange());
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
