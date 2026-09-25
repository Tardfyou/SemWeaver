#include "clang/AST/Expr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/StmtVisitor.h"
#include "clang/AST/Decl.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/Version.h"
#include "clang/Lex/Lexer.h"
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
#include <vector>

using namespace clang;
using namespace ento;

namespace {

static const Expr *ignoreNoise(const Expr *E) {
  if (!E)
    return nullptr;
  return E->IgnoreParenImpCasts();
}

static const DeclRefExpr *asDeclRef(const Expr *E) {
  E = ignoreNoise(E);
  if (!E)
    return nullptr;
  return dyn_cast<DeclRefExpr>(E);
}

static const VarDecl *asVar(const Expr *E) {
  const DeclRefExpr *DRE = asDeclRef(E);
  if (!DRE)
    return nullptr;
  return dyn_cast<VarDecl>(DRE->getDecl());
}

static bool exprReferencesVar(const Expr *E, const VarDecl *Target) {
  if (!E || !Target)
    return false;
  E = ignoreNoise(E);
  if (!E)
    return false;
  if (const auto *DRE = dyn_cast<DeclRefExpr>(E)) {
    return DRE->getDecl() == Target;
  }
  for (const Stmt *Child : E->children()) {
    const auto *ChildExpr = dyn_cast_or_null<Expr>(Child);
    if (exprReferencesVar(ChildExpr, Target))
      return true;
  }
  return false;
}

static bool isSizeofTargetVar(const Expr *E, const VarDecl *Target) {
  E = ignoreNoise(E);
  if (!E || !Target)
    return false;
  const auto *UET = dyn_cast<UnaryExprOrTypeTraitExpr>(E);
  if (!UET || UET->getKind() != UETT_SizeOf || UET->isArgumentType())
    return false;
  return asVar(UET->getArgumentExpr()) == Target;
}

static bool isGreaterThanSizeofVar(const Expr *Cond, const VarDecl *BytesVar,
                                   const VarDecl *ValVar) {
  Cond = ignoreNoise(Cond);
  if (!Cond || !BytesVar || !ValVar)
    return false;

  const auto *BO = dyn_cast<BinaryOperator>(Cond);
  if (!BO)
    return false;

  BinaryOperatorKind Opc = BO->getOpcode();
  const Expr *LHS = BO->getLHS();
  const Expr *RHS = BO->getRHS();

  if (Opc == BO_GT || Opc == BO_GE) {
    return asVar(LHS) == BytesVar && isSizeofTargetVar(RHS, ValVar);
  }
  if (Opc == BO_LT || Opc == BO_LE) {
    return asVar(RHS) == BytesVar && isSizeofTargetVar(LHS, ValVar);
  }
  return false;
}

static bool stmtHasImmediateEscape(const Stmt *S) {
  if (!S)
    return false;
  if (isa<ReturnStmt>(S))
    return true;
  if (const auto *CS = dyn_cast<CompoundStmt>(S)) {
    for (const Stmt *Child : CS->body()) {
      if (isa<ReturnStmt>(Child))
        return true;
    }
  }
  return false;
}

static bool hasGuardForBytesAgainstVal(const Stmt *S, const VarDecl *BytesVar,
                                       const VarDecl *ValVar) {
  if (!S || !BytesVar || !ValVar)
    return false;

  class GuardVisitor : public RecursiveASTVisitor<GuardVisitor> {
    const VarDecl *BytesVar;
    const VarDecl *ValVar;

  public:
    bool Found = false;

    GuardVisitor(const VarDecl *B, const VarDecl *V) : BytesVar(B), ValVar(V) {}

    bool VisitIfStmt(IfStmt *IfS) {
      if (Found || !IfS)
        return true;
      const Expr *Cond = IfS->getCond();
      if (!isGreaterThanSizeofVar(Cond, BytesVar, ValVar))
        return true;
      if (stmtHasImmediateEscape(IfS->getThen()))
        Found = true;
      return true;
    }
  };

  GuardVisitor V(BytesVar, ValVar);
  V.TraverseStmt(const_cast<Stmt *>(S));
  return V.Found;
}

class DecodeLoopVisitor : public RecursiveASTVisitor<DecodeLoopVisitor> {
  ASTContext &ACtx;
  const VarDecl *BytesVar = nullptr;
  const VarDecl *ValVar = nullptr;
  const Stmt *TriggerStmt = nullptr;
  std::vector<const VarDecl *> ByteFeedVars;
  bool HasByteFeed = false;
  bool HasShiftAccumulate = false;

public:
  explicit DecodeLoopVisitor(ASTContext &C) : ACtx(C) {}

  bool VisitWhileStmt(WhileStmt *WS) {
    if (!WS || TriggerStmt)
      return true;

    const VarDecl *LoopBytesVar = findDecrementedVar(WS->getCond());
    if (!LoopBytesVar)
      return true;

    BytesVar = LoopBytesVar;
    ValVar = nullptr;
    ByteFeedVars.clear();
    HasByteFeed = false;
    HasShiftAccumulate = false;
    scanStmt(WS->getBody());

    if (ValVar && HasByteFeed && HasShiftAccumulate)
      TriggerStmt = WS;
    return true;
  }

  const VarDecl *getBytesVar() const { return BytesVar; }
  const VarDecl *getValVar() const { return ValVar; }
  const Stmt *getTriggerStmt() const { return TriggerStmt; }

private:
  void scanStmt(const Stmt *S) {
    if (!S)
      return;
    if (const auto *DS = dyn_cast<DeclStmt>(S))
      scanDeclStmt(DS);
    if (const auto *BO = dyn_cast<BinaryOperator>(S)) {
      if (BO->isAssignmentOp() || BO->isCompoundAssignmentOp())
        scanBinaryOperator(BO);
    }
    for (const Stmt *Child : S->children())
      scanStmt(Child);
  }

  void scanDeclStmt(const DeclStmt *DS) {
    if (!DS)
      return;
    for (const Decl *D : DS->decls()) {
      const auto *VD = dyn_cast_or_null<VarDecl>(D);
      if (!VD || !VD->hasInit())
        continue;
      if (referencesIncrementedNamePointer(VD->getInit())) {
        ByteFeedVars.push_back(VD);
        HasByteFeed = true;
      }
    }
  }

  void scanBinaryOperator(const BinaryOperator *BO) {
    if (!BO)
      return;

    const Expr *LHS = BO->getLHS();
    const Expr *RHS = BO->getRHS();
    const VarDecl *AssignedVar = asVar(LHS);

    if (referencesIncrementedNamePointer(RHS))
      HasByteFeed = true;

    bool FeedsFromByte = referencesIncrementedNamePointer(RHS) ||
                         exprReferencesAnyVar(RHS, ByteFeedVars);
    bool BuildsMultiByteValue = containsOpcode(RHS, BO_Shl) ||
                                containsOpcode(RHS, BO_Or) ||
                                containsOpcode(RHS, BO_Add) ||
                                BO->isCompoundAssignmentOp();

    if (!AssignedVar || !FeedsFromByte || !BuildsMultiByteValue)
      return;

    if (!ValVar)
      ValVar = AssignedVar;
    if (AssignedVar != ValVar)
      return;

    HasByteFeed = true;
    HasShiftAccumulate = true;
  }

  bool containsOpcode(const Expr *E, BinaryOperatorKind K) {
    E = ignoreNoise(E);
    if (!E)
      return false;
    if (const auto *BO = dyn_cast<BinaryOperator>(E)) {
      if (BO->getOpcode() == K)
        return true;
    }
    for (const Stmt *Child : E->children()) {
      const auto *ChildExpr = dyn_cast_or_null<Expr>(Child);
      if (containsOpcode(ChildExpr, K))
        return true;
    }
    return false;
  }

  bool exprReferencesAnyVar(const Expr *E,
                            const std::vector<const VarDecl *> &Vars) {
    for (const VarDecl *VD : Vars) {
      if (exprReferencesVar(E, VD))
        return true;
    }
    return false;
  }

  const VarDecl *findDecrementedVar(const Expr *E) {
    E = ignoreNoise(E);
    if (!E)
      return nullptr;
    if (const auto *UO = dyn_cast<UnaryOperator>(E)) {
      if (UO->getOpcode() == UO_PostDec || UO->getOpcode() == UO_PreDec)
        return asVar(UO->getSubExpr());
    }
    for (const Stmt *Child : E->children()) {
      const auto *ChildExpr = dyn_cast_or_null<Expr>(Child);
      if (const VarDecl *VD = findDecrementedVar(ChildExpr))
        return VD;
    }
    return nullptr;
  }

  bool referencesIncrementedNamePointer(const Expr *E) {
    E = ignoreNoise(E);
    if (!E)
      return false;
    if (const auto *UO = dyn_cast<UnaryOperator>(E)) {
      if (UO->getOpcode() == UO_Deref) {
        const Expr *Sub = ignoreNoise(UO->getSubExpr());
        if (const auto *InnerUO = dyn_cast_or_null<UnaryOperator>(Sub)) {
          if (InnerUO->isIncrementDecrementOp())
            return true;
        }
      }
      if (UO->isIncrementDecrementOp())
        return true;
    }
    for (const Stmt *Child : E->children()) {
      const auto *ChildExpr = dyn_cast_or_null<Expr>(Child);
      if (referencesIncrementedNamePointer(ChildExpr))
        return true;
    }
    return false;
  }
};

class EncodedScalarLengthValidationChecker : public Checker<check::ASTCodeBody> {
  mutable std::unique_ptr<BugType> BT;

public:
  EncodedScalarLengthValidationChecker()
      : BT(std::make_unique<BugType>(this,
                                     "Encoded scalar length validation missing",
                                     "Custom")) {}

  void checkASTCodeBody(const Decl *D, AnalysisManager &, BugReporter &BR) const {
    const auto *FD = dyn_cast_or_null<FunctionDecl>(D);
    if (!FD || !FD->hasBody())
      return;

    ASTContext &ACtx = FD->getASTContext();
    const SourceManager &SM = ACtx.getSourceManager();

    StringRef FileName = SM.getFilename(FD->getLocation());
    if (!FileName.ends_with("binutils/readelf.c") && !FileName.ends_with("readelf.c"))
      return;

    const Stmt *Body = FD->getBody();
    if (!Body)
      return;

    DecodeLoopVisitor V(ACtx);
    V.TraverseStmt(const_cast<Stmt *>(Body));

    const VarDecl *BytesVar = V.getBytesVar();
    const VarDecl *ValVar = V.getValVar();
    const Stmt *Trigger = V.getTriggerStmt();
    if (!BytesVar || !ValVar || !Trigger)
      return;

    if (hasGuardForBytesAgainstVal(Body, BytesVar, ValVar))
      return;

    PathDiagnosticLocation Loc(Trigger->getBeginLoc(), SM);
    BR.EmitBasicReport(FD, this,
                       "Encoded scalar length validation missing",
                       "Security",
                       "A byte count drives per-byte shift/or reconstruction into a scalar value without a guarding bound against sizeof(the destination scalar).",
                       Loc,
                       Trigger->getSourceRange());
  }
};

} // namespace

extern "C" void clang_registerCheckers(CheckerRegistry &Registry) {
  Registry.addChecker<EncodedScalarLengthValidationChecker>(
      "custom.EncodedScalarLengthValidationChecker",
      "Detects missing length validation for encoded scalar reconstruction.",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] = CLANG_VERSION_STRING;
