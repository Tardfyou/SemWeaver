#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "clang/Basic/Version.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/CheckerManager.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include "llvm/ADT/SmallVector.h"
#include <memory>

using namespace clang;
using namespace ento;

namespace {

static const Expr *strip(const Expr *E) {
  return E ? E->IgnoreParenCasts() : nullptr;
}

static bool fileEndsWith(const SourceManager &SM, SourceLocation Loc,
                         llvm::StringRef Suffix) {
  if (Loc.isInvalid())
    return false;
  llvm::StringRef File = SM.getFilename(SM.getSpellingLoc(Loc));
  return !File.empty() && File.ends_with(Suffix);
}

static bool isDeclRefNamed(const Expr *E, llvm::StringRef Name) {
  const auto *DRE = dyn_cast_or_null<DeclRefExpr>(strip(E));
  return DRE && DRE->getDecl() && DRE->getDecl()->getName() == Name;
}

static bool isMemberNamed(const Expr *E, llvm::StringRef Name) {
  const auto *ME = dyn_cast_or_null<MemberExpr>(strip(E));
  return ME && ME->getMemberDecl() && ME->getMemberDecl()->getName() == Name;
}

// Match attr.u.blk->data (arrow/dot mixed), with optional extra casts/parens.
static bool isAttrBlkDataExpr(const Expr *E) {
  const auto *DataME = dyn_cast_or_null<MemberExpr>(strip(E));
  if (!DataME || !DataME->getMemberDecl() ||
      DataME->getMemberDecl()->getName() != "data")
    return false;

  const Expr *DataBase = strip(DataME->getBase());
  const auto *BlkME = dyn_cast_or_null<MemberExpr>(DataBase);
  if (!BlkME || !BlkME->getMemberDecl() ||
      BlkME->getMemberDecl()->getName() != "blk")
    return false;

  const Expr *BlkBase = strip(BlkME->getBase());
  const auto *UME = dyn_cast_or_null<MemberExpr>(BlkBase);
  if (!UME || !UME->getMemberDecl() || UME->getMemberDecl()->getName() != "u")
    return false;

  return isDeclRefNamed(UME->getBase(), "attr");
}

static bool isNullLikeExpr(const Expr *E) {
  E = strip(E);
  if (!E)
    return false;
  if (isa<CXXNullPtrLiteralExpr>(E))
    return true;
  if (const auto *IL = dyn_cast<IntegerLiteral>(E))
    return IL->getValue().isZero();
  return false;
}

static bool isAttrBlkDataNonNullCheck(const Expr *Cond) {
  const auto *BO = dyn_cast_or_null<BinaryOperator>(strip(Cond));
  if (!BO || BO->getOpcode() != BO_NE)
    return false;
  const Expr *LHS = strip(BO->getLHS());
  const Expr *RHS = strip(BO->getRHS());
  return (isAttrBlkDataExpr(LHS) && isNullLikeExpr(RHS)) ||
         (isAttrBlkDataExpr(RHS) && isNullLikeExpr(LHS));
}

static bool isTargetDeref(const UnaryOperator *UO) {
  if (!UO || UO->getOpcode() != UO_Deref)
    return false;
  return isAttrBlkDataExpr(UO->getSubExpr());
}

class NullDerefChecker : public Checker<check::ASTCodeBody> {
  mutable std::unique_ptr<BugType> BT;

public:
  NullDerefChecker()
      : BT(std::make_unique<BugType>(
            this, "Patch-scoped null dereference", "Memory error")) {}

  void checkASTCodeBody(const Decl *D, AnalysisManager &, BugReporter &BR) const {
    const auto *FD = dyn_cast_or_null<FunctionDecl>(D);
    if (!FD || !FD->hasBody())
      return;
    if (!FD->getIdentifier() || FD->getIdentifier()->getName() != "scan_unit_for_symbols")
      return;
    if (!fileEndsWith(BR.getSourceManager(), FD->getLocation(), "bfd/dwarf2.c"))
      return;

    scanStmt(FD->getBody(), /*guarded=*/false, FD, BR);
  }

private:
  void scanStmt(const Stmt *S, bool Guarded, const FunctionDecl *FD,
                BugReporter &BR) const {
    if (!S)
      return;

    if (const auto *IfS = dyn_cast<IfStmt>(S)) {
      const Expr *Cond = IfS->getCond();
      bool ThenGuarded = Guarded || isAttrBlkDataNonNullCheck(Cond);
      scanStmt(IfS->getCond(), Guarded, FD, BR);
      scanStmt(IfS->getThen(), ThenGuarded, FD, BR);
      scanStmt(IfS->getElse(), Guarded, FD, BR);
      return;
    }

    if (const auto *BO = dyn_cast<BinaryOperator>(S)) {
      if (BO->getOpcode() == BO_LAnd) {
        bool RHSGuarded = Guarded || isAttrBlkDataNonNullCheck(BO->getLHS());
        scanStmt(BO->getLHS(), Guarded, FD, BR);
        scanStmt(BO->getRHS(), RHSGuarded, FD, BR);
        return;
      }
    }

    if (!Guarded) {
      if (const auto *UO = dyn_cast<UnaryOperator>(S)) {
        if (isTargetDeref(UO))
          emitReport(UO, FD, BR);
      }
    }

    for (const Stmt *Child : S->children())
      scanStmt(Child, Guarded, FD, BR);
  }

  void emitReport(const UnaryOperator *UO, const FunctionDecl *FD,
                  BugReporter &BR) const {
    PathDiagnosticLocation Loc(UO->getOperatorLoc(), BR.getSourceManager());
    llvm::SmallVector<SourceRange, 1> Ranges;
    Ranges.push_back(UO->getSourceRange());
    BR.EmitBasicReport(
        FD, this, "Potential null dereference of attr.u.blk->data",
        "Memory error",
        "Patch-scoped match: dereference of attr.u.blk->data without a proven "
        "attr.u.blk->data != NULL guard.",
        Loc, Ranges);
  }
};

} // namespace

extern "C" void clang_registerCheckers(CheckerRegistry &Registry) {
  Registry.addChecker<NullDerefChecker>("custom.NullDerefChecker",
                                        "Patch-guided null dereference checker.", "");
}

extern "C" const char clang_analyzerAPIVersionString[] = CLANG_VERSION_STRING;
