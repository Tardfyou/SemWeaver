#include "clang/AST/Expr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/Decl.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/Version.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/CheckerManager.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/MemRegion.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SVals.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include <memory>

using namespace clang;
using namespace ento;

namespace {

static const Expr *ignoreCastsAndParens(const Expr *E) {
  if (!E)
    return nullptr;
  return E->IgnoreParenImpCasts();
}

static const IdentifierInfo *getDirectCalleeII(const CallExpr *CE) {
  if (!CE)
    return nullptr;
  const Expr *CalleeExpr = ignoreCastsAndParens(CE->getCallee());
  if (!CalleeExpr)
    return nullptr;
  if (const auto *DRE = dyn_cast<DeclRefExpr>(CalleeExpr)) {
    const ValueDecl *VD = DRE->getDecl();
    if (!VD)
      return nullptr;
    return VD->getIdentifier();
  }
  return nullptr;
}

static bool isCallNamed(const CallExpr *CE, StringRef Name) {
  const IdentifierInfo *II = getDirectCalleeII(CE);
  if (!II)
    return false;
  return II->getName() == Name;
}

static bool exprReferencesParam(const Expr *E, const ParmVarDecl *Param) {
  E = ignoreCastsAndParens(E);
  if (!E || !Param)
    return false;

  if (const auto *DRE = dyn_cast<DeclRefExpr>(E)) {
    const ValueDecl *VD = DRE->getDecl();
    return VD == Param;
  }

  if (const auto *ME = dyn_cast<MemberExpr>(E)) {
    return exprReferencesParam(ME->getBase(), Param);
  }

  if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(E)) {
    return exprReferencesParam(ASE->getBase(), Param) ||
           exprReferencesParam(ASE->getIdx(), Param);
  }

  if (const auto *CE = dyn_cast<CallExpr>(E)) {
    for (const Expr *Arg : CE->arguments()) {
      if (exprReferencesParam(Arg, Param))
        return true;
    }
    return false;
  }

  return false;
}

static bool isSizeCallOnParam(const CallExpr *CE, const ParmVarDecl *Param) {
  if (!CE || !Param)
    return false;
  if (!isCallNamed(CE, "jas_seq2d_size"))
    return false;
  if (CE->getNumArgs() < 1)
    return false;
  return exprReferencesParam(CE->getArg(0), Param);
}

static bool containsSizeGuardOnParam(const Stmt *S, const ParmVarDecl *Param) {
  if (!S || !Param)
    return false;

  class GuardVisitor : public RecursiveASTVisitor<GuardVisitor> {
    const ParmVarDecl *Param;

  public:
    bool Found = false;

    explicit GuardVisitor(const ParmVarDecl *P) : Param(P) {}

    bool VisitCallExpr(CallExpr *CE) {
      if (!CE || Found)
        return true;
      if (isSizeCallOnParam(CE, Param))
        Found = true;
      return !Found;
    }
  };

  GuardVisitor V(Param);
  V.TraverseStmt(const_cast<Stmt *>(S));
  return V.Found;
}

static bool isNumLvlsPositiveCheck(const Expr *E, const ParmVarDecl *TsfbParam) {
  E = ignoreCastsAndParens(E);
  if (!E || !TsfbParam)
    return false;

  const auto *BO = dyn_cast<BinaryOperator>(E);
  if (!BO || !BO->isComparisonOp())
    return false;

  auto matchesMember = [&](const Expr *Side) -> bool {
    Side = ignoreCastsAndParens(Side);
    const auto *ME = dyn_cast_or_null<MemberExpr>(Side);
    if (!ME)
      return false;
    const ValueDecl *MemberVD = ME->getMemberDecl();
    if (!MemberVD)
      return false;
    const IdentifierInfo *MemberII = MemberVD->getIdentifier();
    if (!MemberII || MemberII->getName() != "numlvls")
      return false;
    return exprReferencesParam(ME->getBase(), TsfbParam);
  };

  auto isZeroLiteral = [&](const Expr *Side) -> bool {
    Side = ignoreCastsAndParens(Side);
    const auto *IL = dyn_cast_or_null<IntegerLiteral>(Side);
    if (!IL)
      return false;
    return IL->getValue() == 0;
  };

  BinaryOperatorKind Op = BO->getOpcode();
  const Expr *LHS = BO->getLHS();
  const Expr *RHS = BO->getRHS();

  if (matchesMember(LHS) && isZeroLiteral(RHS)) {
    return Op == BO_GT || Op == BO_NE || Op == BO_GE;
  }
  if (isZeroLiteral(LHS) && matchesMember(RHS)) {
    return Op == BO_LT || Op == BO_NE || Op == BO_LE;
  }
  return false;
}

static bool containsNumLvlsPositiveCheck(const Stmt *S, const ParmVarDecl *TsfbParam) {
  if (!S || !TsfbParam)
    return false;

  class NumLvlsVisitor : public RecursiveASTVisitor<NumLvlsVisitor> {
    const ParmVarDecl *TsfbParam;

  public:
    bool Found = false;

    explicit NumLvlsVisitor(const ParmVarDecl *P) : TsfbParam(P) {}

    bool VisitBinaryOperator(BinaryOperator *BO) {
      if (!BO || Found)
        return true;
      if (isNumLvlsPositiveCheck(BO, TsfbParam))
        Found = true;
      return !Found;
    }
  };

  NumLvlsVisitor V(TsfbParam);
  V.TraverseStmt(const_cast<Stmt *>(S));
  return V.Found;
}

static bool isGetRefFromParam(const Expr *E, const ParmVarDecl *AParam) {
  E = ignoreCastsAndParens(E);
  const auto *CE = dyn_cast_or_null<CallExpr>(E);
  if (!CE || !AParam)
    return false;
  if (!isCallNamed(CE, "jas_seq2d_getref"))
    return false;
  if (CE->getNumArgs() < 1)
    return false;
  return exprReferencesParam(CE->getArg(0), AParam);
}

class EmptySequenceReferenceGuardChecker : public Checker<check::ASTCodeBody> {
  mutable std::unique_ptr<BugType> BT;

public:
  EmptySequenceReferenceGuardChecker()
      : BT(std::make_unique<BugType>(this,
                                     "empty sequence reference without size guard",
                                     "Custom")) {}

  void checkASTCodeBody(const Decl *D, AnalysisManager &, BugReporter &BR) const;
};

class FunctionBodyVisitor : public RecursiveASTVisitor<FunctionBodyVisitor> {
  const EmptySequenceReferenceGuardChecker *Checker;
  BugReporter &BR;
  const FunctionDecl *FD;
  const ParmVarDecl *TsfbParam;
  const ParmVarDecl *AParam;
  bool Reported = false;

public:
  FunctionBodyVisitor(const EmptySequenceReferenceGuardChecker *Checker,
                      BugReporter &BR, const FunctionDecl *FD,
                      const ParmVarDecl *TsfbParam, const ParmVarDecl *AParam)
      : Checker(Checker), BR(BR), FD(FD), TsfbParam(TsfbParam), AParam(AParam) {}

  bool VisitConditionalOperator(ConditionalOperator *CO) {
    if (!CO || Reported)
      return true;
    analyzeGuardedExpr(CO->getCond(), CO->getTrueExpr());
    return !Reported;
  }

  bool VisitIfStmt(IfStmt *IS) {
    if (!IS || Reported)
      return true;
    analyzeGuardedExpr(IS->getCond(), IS->getThen());
    return !Reported;
  }

private:
  void analyzeGuardedExpr(const Stmt *Cond, const Stmt *Guarded) {
    if (!Cond || !Guarded || !TsfbParam || !AParam || !FD || Reported)
      return;

    if (!containsNumLvlsPositiveCheck(Cond, TsfbParam))
      return;

    if (containsSizeGuardOnParam(Cond, AParam))
      return;

    class SinkVisitor : public RecursiveASTVisitor<SinkVisitor> {
      const ParmVarDecl *AParam;

    public:
      const CallExpr *MatchedCall = nullptr;

      explicit SinkVisitor(const ParmVarDecl *P) : AParam(P) {}

      bool VisitCallExpr(CallExpr *CE) {
        if (!CE || MatchedCall)
          return true;
        if (!isCallNamed(CE, "jpc_tsfb_synthesize2"))
          return true;
        for (const Expr *Arg : CE->arguments()) {
          if (isGetRefFromParam(Arg, AParam)) {
            MatchedCall = CE;
            break;
          }
        }
        return MatchedCall == nullptr;
      }
    };

    SinkVisitor V(AParam);
    V.TraverseStmt(const_cast<Stmt *>(Guarded));
    if (!V.MatchedCall)
      return;

    PathDiagnosticLocation Loc(V.MatchedCall->getBeginLoc(), BR.getSourceManager());
    BR.EmitBasicReport(FD, Checker,
                       "Empty sequence reference passed downstream",
                       "PatchGuided",
                       "This code obtains a reference from sequence parameter 'a' via jas_seq2d_getref and passes it to jpc_tsfb_synthesize2 under a numlvls-positive condition without an explicit jas_seq2d_size(a) non-empty guard.",
                       Loc, V.MatchedCall->getSourceRange());
    Reported = true;
  }
};

void EmptySequenceReferenceGuardChecker::checkASTCodeBody(const Decl *D,
                                                          AnalysisManager &,
                                                          BugReporter &BR) const {
  const auto *FD = dyn_cast_or_null<FunctionDecl>(D);
  if (!FD)
    return;
  const IdentifierInfo *II = FD->getIdentifier();
  if (!II || II->getName() != "jpc_tsfb_synthesize")
    return;
  if (FD->param_size() < 2)
    return;

  const ParmVarDecl *TsfbParam = FD->getParamDecl(0);
  const ParmVarDecl *AParam = FD->getParamDecl(1);
  if (!TsfbParam || !AParam)
    return;

  const Stmt *Body = FD->getBody();
  if (!Body)
    return;

  FunctionBodyVisitor V(this, BR, FD, TsfbParam, AParam);
  V.TraverseStmt(const_cast<Stmt *>(Body));
}

} // namespace

extern "C" void clang_registerCheckers(CheckerRegistry &Registry) {
  Registry.addChecker<EmptySequenceReferenceGuardChecker>(
      "custom.EmptySequenceReferenceGuardChecker",
      "Detects deriving a reference from an empty jas_seq2d_t without a size guard in jpc_tsfb_synthesize.",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] = CLANG_VERSION_STRING;
