#include "clang/AST/Expr.h"
#include "clang/Basic/SourceLocation.h"
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
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include <memory>

using namespace clang;
using namespace ento;

namespace {

static bool isZeroIntegerLiteral(const Expr *E) {
  E = E ? E->IgnoreParenCasts() : nullptr;
  if (const auto *IL = dyn_cast_or_null<IntegerLiteral>(E))
    return IL->getValue().isZero();
  return false;
}

static const Expr *ignoreNoise(const Expr *E) {
  return E ? E->IgnoreParenCasts() : nullptr;
}

static const ValueDecl *getBaseValueDecl(const Expr *E) {
  E = ignoreNoise(E);
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

  return nullptr;
}

static bool exprMentionsDecl(const Expr *E, const ValueDecl *Target) {
  E = ignoreNoise(E);
  if (!E || !Target)
    return false;

  if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
    return DRE->getDecl() == Target;

  if (const auto *ME = dyn_cast<MemberExpr>(E))
    return ME->getMemberDecl() == Target || exprMentionsDecl(ME->getBase(), Target);

  if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(E))
    return exprMentionsDecl(ASE->getBase(), Target) || exprMentionsDecl(ASE->getIdx(), Target);

  if (const auto *BO = dyn_cast<BinaryOperator>(E))
    return exprMentionsDecl(BO->getLHS(), Target) || exprMentionsDecl(BO->getRHS(), Target);

  if (const auto *UO = dyn_cast<UnaryOperator>(E))
    return exprMentionsDecl(UO->getSubExpr(), Target);

  if (const auto *CO = dyn_cast<ConditionalOperator>(E))
    return exprMentionsDecl(CO->getCond(), Target) ||
           exprMentionsDecl(CO->getTrueExpr(), Target) ||
           exprMentionsDecl(CO->getFalseExpr(), Target);

  for (const Stmt *Child : E->children()) {
    if (const auto *ChildExpr = dyn_cast_or_null<Expr>(Child)) {
      if (exprMentionsDecl(ChildExpr, Target))
        return true;
    }
  }

  return false;
}

static bool isZeroComparisonForDecl(const Expr *Cond, const ValueDecl *Target) {
  Cond = ignoreNoise(Cond);
  if (!Cond || !Target)
    return false;

  if (const auto *UO = dyn_cast<UnaryOperator>(Cond)) {
    if (UO->getOpcode() == UO_LNot)
      return exprMentionsDecl(UO->getSubExpr(), Target);
  }

  const auto *BO = dyn_cast<BinaryOperator>(Cond);
  if (!BO)
    return false;

  if (BO->isLogicalOp())
    return isZeroComparisonForDecl(BO->getLHS(), Target) ||
           isZeroComparisonForDecl(BO->getRHS(), Target);

  if (!BO->isComparisonOp())
    return false;

  const Expr *LHS = ignoreNoise(BO->getLHS());
  const Expr *RHS = ignoreNoise(BO->getRHS());

  const bool LeftMentions = exprMentionsDecl(LHS, Target);
  const bool RightMentions = exprMentionsDecl(RHS, Target);
  const bool LeftZero = isZeroIntegerLiteral(LHS);
  const bool RightZero = isZeroIntegerLiteral(RHS);

  switch (BO->getOpcode()) {
  case BO_EQ:
    return (LeftMentions && RightZero) || (RightMentions && LeftZero);
  case BO_LE:
  case BO_LT:
    return RightMentions && LeftZero;
  case BO_GE:
  case BO_GT:
    return LeftMentions && RightZero;
  default:
    return false;
  }
}

static bool isEarlyExitStmt(const Stmt *S) {
  if (!S)
    return false;

  if (isa<ReturnStmt>(S) || isa<BreakStmt>(S) || isa<ContinueStmt>(S) ||
      isa<GotoStmt>(S))
    return true;

  if (const auto *CS = dyn_cast<CompoundStmt>(S)) {
    for (const Stmt *Child : CS->body()) {
      if (isEarlyExitStmt(Child))
        return true;
    }
  }

  return false;
}

class GuardCollector : public ConstStmtVisitor<GuardCollector> {
  llvm::SmallVectorImpl<const ValueDecl *> &Guarded;

public:
  explicit GuardCollector(llvm::SmallVectorImpl<const ValueDecl *> &Guarded)
      : Guarded(Guarded) {}

  void VisitStmt(const Stmt *S) {
    for (const Stmt *Child : S->children()) {
      if (Child)
        Visit(Child);
    }
  }

  void VisitIfStmt(const IfStmt *IfS) {
    const Stmt *Then = IfS->getThen();
    const Stmt *Else = IfS->getElse();

    if (Then && isEarlyExitStmt(Then)) {
      if (const ValueDecl *VD = getBaseValueDecl(IfS->getCond())) {
        if (isZeroComparisonForDecl(IfS->getCond(), VD))
          Guarded.push_back(VD);
      }
    }

    if (Else && isEarlyExitStmt(Else)) {
      const Expr *Cond = ignoreNoise(IfS->getCond());
      if (const auto *UO = dyn_cast_or_null<UnaryOperator>(Cond)) {
        if (UO->getOpcode() == UO_LNot) {
          if (const ValueDecl *VD = getBaseValueDecl(UO->getSubExpr()))
            Guarded.push_back(VD);
        }
      }
    }

    VisitStmt(IfS);
  }
};

class DivisionVisitor : public ConstStmtVisitor<DivisionVisitor> {
  BugReporter &BR;
  const CheckerBase *Checker;
  AnalysisDeclContext *ADC;
  const BugType &BT;
  llvm::SmallVectorImpl<const ValueDecl *> &Guarded;

public:
  DivisionVisitor(BugReporter &BR, const CheckerBase *Checker,
                  AnalysisDeclContext *ADC, const BugType &BT,
                  llvm::SmallVectorImpl<const ValueDecl *> &Guarded)
      : BR(BR), Checker(Checker), ADC(ADC), BT(BT), Guarded(Guarded) {}

  void VisitStmt(const Stmt *S) {
    for (const Stmt *Child : S->children()) {
      if (Child)
        Visit(Child);
    }
  }

  void VisitBinaryOperator(const BinaryOperator *BO) {
    if (!BO)
      return;

    if (BO->getOpcode() != BO_Div && BO->getOpcode() != BO_Rem) {
      VisitStmt(BO);
      return;
    }

    const Expr *Denom = ignoreNoise(BO->getRHS());
    if (!Denom || isZeroIntegerLiteral(Denom)) {
      VisitStmt(BO);
      return;
    }

    const ValueDecl *VD = getBaseValueDecl(Denom);
    if (VD) {
      for (const ValueDecl *GuardedVD : Guarded) {
        if (GuardedVD == VD) {
          VisitStmt(BO);
          return;
        }
      }
    }

    SmallVector<SourceRange, 1> Ranges;
    Ranges.push_back(Denom->getSourceRange());

    PathDiagnosticLocation Loc = PathDiagnosticLocation::createBegin(BO, BR.getSourceManager(), ADC);
    BR.EmitBasicReport(ADC->getDecl(), Checker, BT.getName(), BT.getCategory(),
                       "Potential divide-by-zero: divisor reaches arithmetic without a preceding fail-closed zero check in this function.",
                       Loc, Ranges);

    VisitStmt(BO);
  }
};

class DivideByZeroChecker : public Checker<check::ASTCodeBody> {
  mutable std::unique_ptr<BugType> BT;

public:
  DivideByZeroChecker()
      : BT(std::make_unique<BugType>(this, "Unchecked divisor", "Custom")) {}

  void checkASTCodeBody(const Decl *D, AnalysisManager &Mgr,
                        BugReporter &BR) const {
    const auto *FD = dyn_cast<FunctionDecl>(D);
    if (!FD || !FD->hasBody())
      return;

    AnalysisDeclContext *ADC = Mgr.getAnalysisDeclContext(D);
    if (!ADC)
      return;

    llvm::SmallVector<const ValueDecl *, 16> Guarded;
    GuardCollector GC(Guarded);
    GC.Visit(FD->getBody());

    DivisionVisitor DV(BR, this, ADC, *BT, Guarded);
    DV.Visit(FD->getBody());
  }
};

} // namespace

extern "C" void clang_registerCheckers(CheckerRegistry &Registry) {
  Registry.addChecker<DivideByZeroChecker>(
      "custom.DivideByZeroChecker",
      "Detect division or modulo by an input-derived value lacking a fail-closed zero guard.",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] = CLANG_VERSION_STRING;
