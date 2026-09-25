#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/StmtVisitor.h"
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

static StringRef getCalleeName(const CallExpr *CE) {
  if (!CE)
    return StringRef();
  const Expr *Callee = ignoreCastsAndParens(CE->getCallee());
  if (!Callee)
    return StringRef();
  if (const auto *DRE = dyn_cast<DeclRefExpr>(Callee)) {
    const ValueDecl *VD = DRE->getDecl();
    if (!VD)
      return StringRef();
    const IdentifierInfo *II = VD->getIdentifier();
    if (!II)
      return StringRef();
    return II->getName();
  }
  return StringRef();
}

static bool isDeclRefNamed(const Expr *E, StringRef Name) {
  const Expr *Base = ignoreCastsAndParens(E);
  if (!Base)
    return false;
  const auto *DRE = dyn_cast<DeclRefExpr>(Base);
  if (!DRE)
    return false;
  const ValueDecl *VD = DRE->getDecl();
  if (!VD)
    return false;
  const IdentifierInfo *II = VD->getIdentifier();
  if (!II)
    return false;
  return II->getName() == Name;
}

static bool isIntegerLiteralValue(const Expr *E, llvm::APSInt &Out) {
  const Expr *Base = ignoreCastsAndParens(E);
  if (!Base)
    return false;
  const auto *IL = dyn_cast<IntegerLiteral>(Base);
  if (!IL)
    return false;
  Out = IL->getValue();
  return true;
}

static bool isNPixPositiveGuardExpr(const Expr *E) {
  const Expr *Base = ignoreCastsAndParens(E);
  if (!Base)
    return false;
  const auto *BO = dyn_cast<BinaryOperator>(Base);
  if (!BO)
    return false;
  if (!(BO->getOpcode() == BO_GT || BO->getOpcode() == BO_GE))
    return false;

  llvm::APSInt Val;
  if (isDeclRefNamed(BO->getLHS(), "npix") && isIntegerLiteralValue(BO->getRHS(), Val))
    return Val.isSigned() ? Val.getSExtValue() >= 0 : Val.getZExtValue() >= 0;
  if (isDeclRefNamed(BO->getRHS(), "npix") && isIntegerLiteralValue(BO->getLHS(), Val))
    return Val == 0;
  return false;
}

static bool isStackTopMinusStackp(const Expr *E) {
  const Expr *Base = ignoreCastsAndParens(E);
  if (!Base)
    return false;
  const auto *BO = dyn_cast<BinaryOperator>(Base);
  if (!BO || BO->getOpcode() != BO_Sub)
    return false;
  return isDeclRefNamed(BO->getLHS(), "stack_top") && isDeclRefNamed(BO->getRHS(), "stackp");
}

static bool isNPixVsRequestedLenGuardExpr(const Expr *E) {
  const Expr *Base = ignoreCastsAndParens(E);
  if (!Base)
    return false;
  const auto *BO = dyn_cast<BinaryOperator>(Base);
  if (!BO)
    return false;
  if (!(BO->getOpcode() == BO_GE || BO->getOpcode() == BO_GT))
    return false;
  return isDeclRefNamed(BO->getLHS(), "npix") && isStackTopMinusStackp(BO->getRHS());
}

class RemainingPixelVisitor : public RecursiveASTVisitor<RemainingPixelVisitor> {
  BugReporter &BR;
  const CheckerBase *Checker;
  const BugType &BT;
  const FunctionDecl *TargetFD;
  bool SeenSingleGuard = false;
  bool SeenBulkGuard = false;

public:
  RemainingPixelVisitor(BugReporter &BR, const CheckerBase *Checker,
                        const BugType &BT, const FunctionDecl *FD)
      : BR(BR), Checker(Checker), BT(BT), TargetFD(FD) {}

  bool VisitIfStmt(IfStmt *IfS) {
    if (!IfS)
      return true;
    const Expr *Cond = IfS->getCond();
    if (!Cond)
      return true;
    if (isNPixPositiveGuardExpr(Cond))
      SeenSingleGuard = true;
    if (isNPixVsRequestedLenGuardExpr(Cond))
      SeenBulkGuard = true;
    return true;
  }

  bool VisitCallExpr(CallExpr *CE) {
    if (!CE || !TargetFD)
      return true;

    StringRef Name = getCalleeName(CE);
    if (Name.empty())
      return true;

    SourceManager &SM = BR.getSourceManager();
    SourceLocation Loc = CE->getExprLoc();
    if (Loc.isInvalid())
      return true;
    if (!SM.isWrittenInMainFile(SM.getExpansionLoc(Loc)))
      return true;

    if (Name == "WritePixel") {
      if (CE->getNumArgs() >= 3 && !SeenSingleGuard) {
        PathDiagnosticLocation PLoc(CE->getBeginLoc(), SM);
        BR.EmitBasicReport(TargetFD, Checker,
                           "remaining pixel budget unchecked before single-pixel write",
                           "Memory safety",
                           "Call to WritePixel appears before any local guard requiring npix > 0, which matches the patched out-of-bounds write pattern.",
                           PLoc, CE->getSourceRange());
      }
      return true;
    }

    if (Name == "WritePixels") {
      if (CE->getNumArgs() >= 4) {
        const Expr *LenArg = CE->getArg(3);
        bool UsesFullStackLen = isStackTopMinusStackp(LenArg);
        bool UsesNPixLen = isDeclRefNamed(LenArg, "npix");
        if (UsesFullStackLen && !SeenBulkGuard) {
          PathDiagnosticLocation PLoc(CE->getBeginLoc(), SM);
          BR.EmitBasicReport(TargetFD, Checker,
                             "remaining pixel budget unchecked before bulk pixel write",
                             "Memory safety",
                             "Call to WritePixels uses stack_top - stackp as the requested length without any prior local guard requiring npix >= stack_top - stackp, which matches the patched out-of-bounds write pattern.",
                             PLoc, CE->getSourceRange());
        } else if (UsesNPixLen) {
          return true;
        }
      }
      return true;
    }

    return true;
  }
};

class RemainingPixelBoundsWriteChecker : public Checker<check::ASTCodeBody> {
  mutable std::unique_ptr<BugType> BT;

public:
  RemainingPixelBoundsWriteChecker()
      : BT(std::make_unique<BugType>(this,
                                     "Remaining pixel budget write overflow",
                                     "Memory safety")) {}

  void checkASTCodeBody(const Decl *D, AnalysisManager &, BugReporter &BR) const {
    if (!D)
      return;
    const auto *FD = dyn_cast<FunctionDecl>(D);
    if (!FD)
      return;
    const Stmt *Body = FD->getBody();
    if (!Body)
      return;

    const IdentifierInfo *II = FD->getIdentifier();
    if (!II)
      return;

    SourceManager &SM = BR.getSourceManager();
    SourceLocation Loc = FD->getLocation();
    if (Loc.isInvalid())
      return;
    SourceLocation FileLoc = SM.getExpansionLoc(Loc);
    if (!SM.isWrittenInMainFile(FileLoc))
      return;

    StringRef FileName = SM.getFilename(FileLoc);
    if (FileName.empty() || !FileName.ends_with("ngiflib.c"))
      return;

    RemainingPixelVisitor Visitor(BR, this, *BT, FD);
    Visitor.TraverseStmt(const_cast<Stmt *>(Body));
  }
};

} // namespace

extern "C" void clang_registerCheckers(CheckerRegistry &Registry) {
  Registry.addChecker<RemainingPixelBoundsWriteChecker>(
      "custom.RemainingPixelBoundsWriteChecker",
      "Detects unchecked WritePixel/WritePixels calls against remaining pixel budget.",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] = CLANG_VERSION_STRING;
