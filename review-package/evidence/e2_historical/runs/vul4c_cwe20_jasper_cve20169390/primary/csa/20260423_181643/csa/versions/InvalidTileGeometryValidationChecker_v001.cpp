#include "clang/AST/Expr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
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
#include "llvm/ADT/StringRef.h"
#include <memory>

using namespace clang;
using namespace ento;

namespace {

static const Expr *ignoreCastsAndParens(const Expr *E) {
  if (!E)
    return nullptr;
  return E->IgnoreParenImpCasts();
}

static StringRef getMemberName(const Expr *E) {
  E = ignoreCastsAndParens(E);
  const auto *ME = dyn_cast_or_null<MemberExpr>(E);
  if (!ME)
    return StringRef();
  const ValueDecl *VD = ME->getMemberDecl();
  if (!VD)
    return StringRef();
  return VD->getName();
}

static const Expr *getMemberBase(const Expr *E) {
  E = ignoreCastsAndParens(E);
  const auto *ME = dyn_cast_or_null<MemberExpr>(E);
  if (!ME)
    return nullptr;
  return ignoreCastsAndParens(ME->getBase());
}

static const ValueDecl *getBaseDecl(const Expr *E) {
  E = ignoreCastsAndParens(E);
  if (!E)
    return nullptr;
  if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
    return DRE->getDecl();
  if (const auto *ME = dyn_cast<MemberExpr>(E))
    return ME->getMemberDecl();
  return nullptr;
}

static bool sameBaseObject(const Expr *A, const Expr *B) {
  const ValueDecl *DA = getBaseDecl(A);
  const ValueDecl *DB = getBaseDecl(B);
  return DA && DB && DA == DB;
}

static bool isTrackedFieldRef(const Expr *E, StringRef FieldA, StringRef FieldB,
                              const Expr *&BaseOut, StringRef &FieldOut) {
  E = ignoreCastsAndParens(E);
  const auto *ME = dyn_cast_or_null<MemberExpr>(E);
  if (!ME)
    return false;
  const ValueDecl *VD = ME->getMemberDecl();
  if (!VD)
    return false;
  StringRef Name = VD->getName();
  if (Name != FieldA && Name != FieldB)
    return false;
  BaseOut = getMemberBase(E);
  FieldOut = Name;
  return BaseOut != nullptr;
}

static bool isRelevantComparison(const BinaryOperator *BO) {
  if (!BO)
    return false;
  if (!(BO->getOpcode() == BO_GE || BO->getOpcode() == BO_GT ||
        BO->getOpcode() == BO_LE || BO->getOpcode() == BO_LT))
    return false;

  const Expr *LHS = nullptr;
  const Expr *RHS = nullptr;
  StringRef LName;
  StringRef RName;
  if (!isTrackedFieldRef(BO->getLHS(), "tilexoff", "tileyoff", LHS, LName) &&
      !isTrackedFieldRef(BO->getLHS(), "width", "height", LHS, LName))
    return false;
  if (!isTrackedFieldRef(BO->getRHS(), "tilexoff", "tileyoff", RHS, RName) &&
      !isTrackedFieldRef(BO->getRHS(), "width", "height", RHS, RName))
    return false;
  if (!sameBaseObject(LHS, RHS))
    return false;

  if ((LName == "tilexoff" && RName == "width") ||
      (LName == "tileyoff" && RName == "height") ||
      (LName == "width" && RName == "tilexoff") ||
      (LName == "height" && RName == "tileyoff"))
    return true;
  return false;
}

class GeometryGuardVisitor : public RecursiveASTVisitor<GeometryGuardVisitor> {
  SourceManager &SM;
  bool HasTileXGuard = false;
  bool HasTileYGuard = false;
  bool HasFailureAction = false;
  bool InTargetFunction = false;

  bool stmtContainsFailureAction(const Stmt *S) {
    if (!S)
      return false;
    class FailureVisitor : public RecursiveASTVisitor<FailureVisitor> {
      bool Found = false;
    public:
      bool VisitReturnStmt(ReturnStmt *RS) {
        if (!RS)
          return true;
        const Expr *Ret = RS->getRetValue();
        Ret = ignoreCastsAndParens(Ret);
        if (const auto *IL = dyn_cast_or_null<IntegerLiteral>(Ret)) {
          if (IL->getValue().isNegative() || IL->getValue() == 1)
            Found = true;
        } else if (!Ret) {
          Found = true;
        }
        return true;
      }
      bool VisitCallExpr(CallExpr *CE) {
        if (!CE)
          return true;
        const FunctionDecl *FD = CE->getDirectCallee();
        if (!FD)
          return true;
        StringRef Name = FD->getName();
        if (Name == "jas_eprintf")
          Found = true;
        return true;
      }
      bool found() const { return Found; }
    } FV;
    FV.TraverseStmt(const_cast<Stmt *>(S));
    return FV.found();
  }

  void recordComparison(const BinaryOperator *BO) {
    if (!BO || !isRelevantComparison(BO))
      return;

    const Expr *LBase = nullptr;
    const Expr *RBase = nullptr;
    StringRef LName;
    StringRef RName;
    bool LOk = isTrackedFieldRef(BO->getLHS(), "tilexoff", "tileyoff", LBase, LName) ||
               isTrackedFieldRef(BO->getLHS(), "width", "height", LBase, LName);
    bool ROk = isTrackedFieldRef(BO->getRHS(), "tilexoff", "tileyoff", RBase, RName) ||
               isTrackedFieldRef(BO->getRHS(), "width", "height", RBase, RName);
    if (!LOk || !ROk || !sameBaseObject(LBase, RBase))
      return;

    BinaryOperatorKind Op = BO->getOpcode();
    if ((LName == "tilexoff" && RName == "width" && (Op == BO_GE || Op == BO_GT)) ||
        (LName == "width" && RName == "tilexoff" && (Op == BO_LE || Op == BO_LT)))
      HasTileXGuard = true;
    if ((LName == "tileyoff" && RName == "height" && (Op == BO_GE || Op == BO_GT)) ||
        (LName == "height" && RName == "tileyoff" && (Op == BO_LE || Op == BO_LT)))
      HasTileYGuard = true;
  }

public:
  explicit GeometryGuardVisitor(SourceManager &SM) : SM(SM) {}

  bool VisitFunctionDecl(FunctionDecl *FD) {
    if (!FD)
      return true;
    StringRef Name = FD->getName();
    InTargetFunction = (Name == "jpc_dec_process_siz");
    return true;
  }

  bool VisitIfStmt(IfStmt *IS) {
    if (!IS || !InTargetFunction)
      return true;
    const Expr *Cond = IS->getCond();
    Cond = ignoreCastsAndParens(Cond);
    if (!Cond)
      return true;

    class CondVisitor : public RecursiveASTVisitor<CondVisitor> {
      GeometryGuardVisitor &Owner;
    public:
      explicit CondVisitor(GeometryGuardVisitor &Owner) : Owner(Owner) {}
      bool VisitBinaryOperator(BinaryOperator *BO) {
        Owner.recordComparison(BO);
        return true;
      }
    } CV(*this);
    CV.TraverseStmt(const_cast<Expr *>(Cond));

    if (stmtContainsFailureAction(IS->getThen()))
      HasFailureAction = true;
    return true;
  }

  bool hasCompleteGuard() const {
    return HasTileXGuard && HasTileYGuard && HasFailureAction;
  }
};

class InvalidTileGeometryValidationChecker : public Checker<check::ASTCodeBody> {
  mutable std::unique_ptr<BugType> BT;

public:
  InvalidTileGeometryValidationChecker()
      : BT(std::make_unique<BugType>(this, "Missing invalid tile geometry validation",
                                     "Custom")) {}

  void checkASTCodeBody(const Decl *D, AnalysisManager &, BugReporter &BR) const {
    const auto *FD = dyn_cast_or_null<FunctionDecl>(D);
    if (!FD)
      return;
    if (!FD->hasBody())
      return;

    StringRef FuncName = FD->getName();
    if (FuncName != "jpc_dec_process_siz")
      return;

    const SourceManager &SM = BR.getSourceManager();
    SourceLocation Loc = FD->getLocation();
    if (Loc.isInvalid())
      return;
    StringRef FileName = SM.getFilename(SM.getSpellingLoc(Loc));
    if (!FileName.ends_with("src/libjasper/jpc/jpc_dec.c"))
      return;

    GeometryGuardVisitor V(const_cast<SourceManager &>(SM));
    V.TraverseDecl(const_cast<FunctionDecl *>(FD));
    if (V.hasCompleteGuard())
      return;

    const Stmt *Body = FD->getBody();
    if (!Body)
      return;

    PathDiagnosticLocation ReportLoc(Body->getBeginLoc(), SM);
    BR.EmitBasicReport(FD, this, "Missing invalid tile geometry validation", "Logic error",
                       "Decoded SIZ parameters are accepted without rejecting the case where tile origins lie outside the image area; missing checks for tilexoff >= width and tileyoff >= height can permit invalid tile/image geometry.",
                       ReportLoc, Body->getSourceRange());
  }
};

} // namespace

extern "C" void clang_registerCheckers(CheckerRegistry &Registry) {
  Registry.addChecker<InvalidTileGeometryValidationChecker>(
      "custom.InvalidTileGeometryValidationChecker",
      "Detects missing validation for tile origins outside image bounds.", "");
}

extern "C" const char clang_analyzerAPIVersionString[] = CLANG_VERSION_STRING;
