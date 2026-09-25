#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/Decl.h"
#include "clang/AST/RecursiveASTVisitor.h"
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
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringRef.h"
#include <memory>

using namespace clang;
using namespace ento;

namespace {

static const Expr *ignoreParenCast(const Expr *E) {
  if (!E)
    return nullptr;
  return E->IgnoreParenImpCasts();
}

static const ValueDecl *getBaseDecl(const Expr *E) {
  E = ignoreParenCast(E);
  if (!E)
    return nullptr;

  if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
    return DRE->getDecl();

  if (const auto *ME = dyn_cast<MemberExpr>(E))
    return ME->getMemberDecl();

  return nullptr;
}

static bool exprRefsDecl(const Expr *E, const ValueDecl *Target) {
  if (!E || !Target)
    return false;

  E = ignoreParenCast(E);
  if (!E)
    return false;

  if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
    return DRE->getDecl() == Target;

  if (const auto *ME = dyn_cast<MemberExpr>(E))
    return ME->getMemberDecl() == Target;

  if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(E))
    return exprRefsDecl(ASE->getBase(), Target) || exprRefsDecl(ASE->getIdx(), Target);

  if (const auto *BO = dyn_cast<BinaryOperator>(E))
    return exprRefsDecl(BO->getLHS(), Target) || exprRefsDecl(BO->getRHS(), Target);

  if (const auto *UO = dyn_cast<UnaryOperator>(E))
    return exprRefsDecl(UO->getSubExpr(), Target);

  if (const auto *CO = dyn_cast<ConditionalOperator>(E))
    return exprRefsDecl(CO->getCond(), Target) || exprRefsDecl(CO->getTrueExpr(), Target) ||
           exprRefsDecl(CO->getFalseExpr(), Target);

  if (const auto *CE = dyn_cast<CallExpr>(E)) {
    for (const Expr *Arg : CE->arguments()) {
      if (exprRefsDecl(Arg, Target))
        return true;
    }
    return exprRefsDecl(CE->getCallee(), Target);
  }

  if (const auto *ICE = dyn_cast<ImplicitCastExpr>(E))
    return exprRefsDecl(ICE->getSubExpr(), Target);

  if (const auto *CCE = dyn_cast<CStyleCastExpr>(E))
    return exprRefsDecl(CCE->getSubExpr(), Target);

  return false;
}

static bool isImageColormapBase(const Expr *Base) {
  Base = ignoreParenCast(Base);
  if (!Base)
    return false;

  const auto *ME = dyn_cast<MemberExpr>(Base);
  if (!ME)
    return false;

  const ValueDecl *Member = ME->getMemberDecl();
  if (!Member)
    return false;

  if (Member->getName() != "colormap")
    return false;

  const Expr *Obj = ignoreParenCast(ME->getBase());
  if (!Obj)
    return false;

  if (const auto *DRE = dyn_cast<DeclRefExpr>(Obj))
    return DRE->getDecl() && DRE->getDecl()->getName() == "image";

  if (const auto *UO = dyn_cast<UnaryOperator>(Obj))
    return isImageColormapBase(UO->getSubExpr());

  return false;
}

static const ValueDecl *getVerifiedColormapIndexDecl(const Stmt *S) {
  if (!S)
    return nullptr;

  const auto *CS = dyn_cast<CallExpr>(ignoreParenCast(dyn_cast<Expr>(S)));
  if (!CS)
    return nullptr;

  const FunctionDecl *FD = CS->getDirectCallee();
  if (!FD || FD->getName() != "VerifyColormapIndex" || CS->getNumArgs() < 2)
    return nullptr;

  const Expr *Arg0 = ignoreParenCast(CS->getArg(0));
  const Expr *Arg1 = ignoreParenCast(CS->getArg(1));
  if (!Arg0 || !Arg1)
    return nullptr;

  const auto *ImgRef = dyn_cast<DeclRefExpr>(Arg0);
  if (!ImgRef || !ImgRef->getDecl() || ImgRef->getDecl()->getName() != "image")
    return nullptr;

  return getBaseDecl(Arg1);
}

static const ValueDecl *getAssignedDecl(const Stmt *S) {
  const auto *E = dyn_cast_or_null<Expr>(S);
  if (!E)
    return nullptr;

  E = ignoreParenCast(E);
  if (const auto *BO = dyn_cast<BinaryOperator>(E)) {
    if (BO->isAssignmentOp())
      return getBaseDecl(BO->getLHS());
  }

  if (const auto *UO = dyn_cast<UnaryOperator>(E)) {
    if (UO->isIncrementDecrementOp())
      return getBaseDecl(UO->getSubExpr());
  }

  return nullptr;
}

class ColormapIndexBoundsChecker : public Checker<check::ASTCodeBody> {
  mutable std::unique_ptr<BugType> BT;

public:
  ColormapIndexBoundsChecker()
      : BT(std::make_unique<BugType>(this, "Colormap index bounds", "Custom")) {}

  void checkASTCodeBody(const Decl *D, AnalysisManager &, BugReporter &BR) const;

private:
  void scanStmt(const Stmt *S, BugReporter &BR, const Decl *D) const;
  void scanCompound(const CompoundStmt *CS, BugReporter &BR, const Decl *D) const;
  void maybeReportArrayRead(const ArraySubscriptExpr *ASE,
                            const llvm::SmallPtrSetImpl<const ValueDecl *> &ValidatedIndices,
                            BugReporter &BR, const Decl *D) const;
};

void ColormapIndexBoundsChecker::checkASTCodeBody(const Decl *D, AnalysisManager &, BugReporter &BR) const {
  if (!D)
    return;

  const auto *FD = dyn_cast<FunctionDecl>(D);
  if (!FD)
    return;

  const Stmt *Body = FD->getBody();
  if (!Body)
    return;

  const SourceManager &SM = BR.getSourceManager();
  SourceLocation Loc = FD->getLocation();
  if (Loc.isInvalid())
    return;

  StringRef FileName = SM.getFilename(SM.getSpellingLoc(Loc));
  if (!FileName.ends_with("coders/sun.c") && !FileName.ends_with("sun.c"))
    return;

  scanStmt(Body, BR, D);
}

void ColormapIndexBoundsChecker::scanStmt(const Stmt *S, BugReporter &BR, const Decl *D) const {
  if (!S)
    return;

  if (const auto *CS = dyn_cast<CompoundStmt>(S)) {
    scanCompound(CS, BR, D);
    return;
  }

  for (const Stmt *Child : S->children()) {
    if (Child)
      scanStmt(Child, BR, D);
  }
}

void ColormapIndexBoundsChecker::scanCompound(const CompoundStmt *CS, BugReporter &BR, const Decl *D) const {
  if (!CS)
    return;

  llvm::SmallPtrSet<const ValueDecl *, 8> ValidatedIndices;
  for (const Stmt *Child : CS->body()) {
    if (!Child)
      continue;

    if (const ValueDecl *Assigned = getAssignedDecl(Child))
      ValidatedIndices.erase(Assigned);

    if (const ValueDecl *Verified = getVerifiedColormapIndexDecl(Child)) {
      ValidatedIndices.insert(Verified);
      continue;
    }

    if (const auto *Inner = dyn_cast<CompoundStmt>(Child)) {
      scanCompound(Inner, BR, D);
      continue;
    }

    class LocalVisitor : public RecursiveASTVisitor<LocalVisitor> {
      const ColormapIndexBoundsChecker *Checker;
      const llvm::SmallPtrSetImpl<const ValueDecl *> &ValidatedIndices;
      BugReporter &BR;
      const Decl *D;

    public:
      LocalVisitor(const ColormapIndexBoundsChecker *Checker,
                   const llvm::SmallPtrSetImpl<const ValueDecl *> &ValidatedIndices,
                   BugReporter &BR, const Decl *D)
          : Checker(Checker), ValidatedIndices(ValidatedIndices), BR(BR), D(D) {}

      bool VisitArraySubscriptExpr(ArraySubscriptExpr *ASE) {
        Checker->maybeReportArrayRead(ASE, ValidatedIndices, BR, D);
        return true;
      }
    } Visitor(this, ValidatedIndices, BR, D);

    Visitor.TraverseStmt(const_cast<Stmt *>(Child));
  }
}

void ColormapIndexBoundsChecker::maybeReportArrayRead(
    const ArraySubscriptExpr *ASE,
    const llvm::SmallPtrSetImpl<const ValueDecl *> &ValidatedIndices,
    BugReporter &BR,
    const Decl *D) const {
  if (!ASE || !D)
    return;

  const Expr *Base = ignoreParenCast(ASE->getBase());
  const Expr *Idx = ignoreParenCast(ASE->getIdx());
  if (!Base || !Idx)
    return;

  if (!isImageColormapBase(Base))
    return;

  const ValueDecl *IndexDecl = getBaseDecl(Idx);
  if (!IndexDecl)
    return;

  if (ValidatedIndices.count(IndexDecl))
    return;

  const SourceManager &SM = BR.getSourceManager();
  SourceLocation Loc = ASE->getExprLoc();
  if (Loc.isInvalid())
    return;

  PathDiagnosticLocation PLoc(Loc, SM);
  BR.EmitBasicReport(D, this, "Missing colormap index validation", "Custom",
                     "Array read from image->colormap uses an index without a preceding VerifyColormapIndex(image, index) check.",
                     PLoc, ASE->getSourceRange());
}

} // namespace

extern "C" void clang_registerCheckers(CheckerRegistry &Registry) {
  Registry.addChecker<ColormapIndexBoundsChecker>("custom.ColormapIndexBoundsChecker",
                                                  "Detects unchecked image->colormap[index] reads in sun decoder paths.",
                                                  "");
}

extern "C" const char clang_analyzerAPIVersionString[] = CLANG_VERSION_STRING;
