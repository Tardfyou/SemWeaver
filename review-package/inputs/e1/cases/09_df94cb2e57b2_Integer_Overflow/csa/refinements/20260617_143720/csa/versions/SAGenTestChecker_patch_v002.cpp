// Detect 32-bit truncation of sector/capacity quantities before accounting sinks.
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"
#include <memory>

using namespace clang;
using namespace ento;

namespace {

class SAGenTestChecker;

static const Expr *ignore(const Expr *E) {
  return E ? E->IgnoreParenImpCasts() : nullptr;
}

static StringRef calleeName(const CallExpr *CE) {
  if (!CE)
    return StringRef();
  if (const FunctionDecl *FD = CE->getDirectCallee())
    return FD->getName();
  return StringRef();
}

static bool isUnsigned32(QualType QT, ASTContext &Ctx) {
  QT = QT.getCanonicalType();
  return QT->isUnsignedIntegerType() && Ctx.getTypeSize(QT) <= 32;
}

static bool isWideInteger(QualType QT, ASTContext &Ctx) {
  QT = QT.getCanonicalType();
  return QT->isIntegerType() && Ctx.getTypeSize(QT) > 32;
}

class ExprUseVisitor : public RecursiveASTVisitor<ExprUseVisitor> {
  const ValueDecl *Target;
  bool Found = false;

public:
  explicit ExprUseVisitor(const ValueDecl *Target) : Target(Target) {}

  bool VisitDeclRefExpr(DeclRefExpr *DRE) {
    if (DRE->getDecl() == Target)
      Found = true;
    return !Found;
  }

  bool found() const { return Found; }
};

static bool exprReferencesDecl(const Expr *E, const ValueDecl *VD) {
  if (!E || !VD)
    return false;
  ExprUseVisitor Visitor(VD);
  Visitor.TraverseStmt(const_cast<Expr *>(E));
  return Visitor.found();
}

class WideExprVisitor : public RecursiveASTVisitor<WideExprVisitor> {
  ASTContext &Ctx;
  bool Found = false;

public:
  explicit WideExprVisitor(ASTContext &Ctx) : Ctx(Ctx) {}

  bool VisitExpr(Expr *E) {
    if (isWideInteger(E->getType(), Ctx))
      Found = true;
    return !Found;
  }

  bool found() const { return Found; }
};

static bool containsWideIntegerExpr(const Expr *E, ASTContext &Ctx) {
  if (!E)
    return false;
  WideExprVisitor Visitor(Ctx);
  Visitor.TraverseStmt(const_cast<Expr *>(E));
  return Visitor.found();
}

class FunctionRoleVisitor : public RecursiveASTVisitor<FunctionRoleVisitor> {
  ASTContext &Ctx;
  const ParmVarDecl *Param;
  bool UsedInAccountingSink = false;
  bool BoundedWithWideCapacity = false;

public:
  FunctionRoleVisitor(ASTContext &Ctx, const ParmVarDecl *Param)
      : Ctx(Ctx), Param(Param) {}

  bool VisitCallExpr(CallExpr *CE) {
    StringRef Name = calleeName(CE);

    if (Name == "bch2_disk_reservation_get" || Name == "bch2_key_resize") {
      for (const Expr *Arg : CE->arguments())
        if (exprReferencesDecl(Arg, Param))
          UsedInAccountingSink = true;
    }

    if ((Name == "min" || Name == "min_t") && CE->getNumArgs() >= 2) {
      bool HasParam = false;
      bool HasWide = false;
      for (const Expr *Arg : CE->arguments()) {
        HasParam |= exprReferencesDecl(Arg, Param);
        HasWide |= containsWideIntegerExpr(Arg, Ctx);
      }
      if (HasParam && HasWide)
        BoundedWithWideCapacity = true;
    }

    return true;
  }

  bool hasSectorCapacityRole() const {
    return UsedInAccountingSink && BoundedWithWideCapacity;
  }
};

class LocalTruncationVisitor : public RecursiveASTVisitor<LocalTruncationVisitor> {
  ASTContext &Ctx;
  BugReporter &BR;
  const CheckerBase *Checker;
  BugType &BugTy;
  llvm::SmallPtrSet<const VarDecl *, 8> WideInitializedUnsigned;
  llvm::SmallPtrSet<const VarDecl *, 8> Reported;

public:
  LocalTruncationVisitor(ASTContext &Ctx, BugReporter &BR,
                         const CheckerBase *Checker, BugType &BugTy)
      : Ctx(Ctx), BR(BR), Checker(Checker), BugTy(BugTy) {}

  bool VisitVarDecl(VarDecl *VD) {
    if (isUnsigned32(VD->getType(), Ctx) && containsWideIntegerExpr(VD->getInit(), Ctx))
      WideInitializedUnsigned.insert(VD->getCanonicalDecl());
    return true;
  }

  bool VisitCallExpr(CallExpr *CE) {
    StringRef Name = calleeName(CE);
    bool IsAccountingSink = Name == "bch2_disk_reservation_get" ||
                            Name == "bch2_key_resize";
    bool IsNarrowDiagnostic = false;

    if (Name == "bch2_trans_inconsistent") {
      for (const Expr *Arg : CE->arguments()) {
        const Expr *Stripped = ignore(Arg);
        if (const auto *SL = dyn_cast_or_null<StringLiteral>(Stripped))
          IsNarrowDiagnostic |= SL->getString().contains("%u") &&
                                !SL->getString().contains("%llu");
      }
    }

    if (!IsAccountingSink && !IsNarrowDiagnostic)
      return true;

    for (const Expr *Arg : CE->arguments()) {
      for (const VarDecl *VD : WideInitializedUnsigned) {
        if (!Reported.count(VD) && exprReferencesDecl(Arg, VD)) {
          report(VD, IsAccountingSink
                         ? "32-bit temporary truncates a wider sector reservation before accounting"
                         : "32-bit temporary truncates a wider sector reservation before it is reported");
          Reported.insert(VD);
        }
      }
    }
    return true;
  }

private:
  void report(const VarDecl *VD, StringRef Msg) {
    PathDiagnosticLocation Loc = PathDiagnosticLocation::createBegin(VD, BR.getSourceManager());
    auto Report = std::make_unique<BasicBugReport>(BugTy, Msg, Loc);
    Report->addRange(VD->getSourceRange());
    BR.emitReport(std::move(Report));
  }
};

class SAGenTestChecker : public Checker<check::ASTCodeBody> {
  mutable std::unique_ptr<BugType> BugTy;

public:
  SAGenTestChecker()
      : BugTy(std::make_unique<BugType>(this, "32-bit sector capacity truncation",
                                        "Integer Overflow")) {}

  void checkASTCodeBody(const Decl *D, AnalysisManager &Mgr, BugReporter &BR) const {
    const auto *FD = dyn_cast<FunctionDecl>(D);
    if (!FD || !FD->hasBody())
      return;

    ASTContext &Ctx = BR.getContext();
    for (const ParmVarDecl *Param : FD->parameters()) {
      if (!isUnsigned32(Param->getType(), Ctx))
        continue;
      FunctionRoleVisitor RoleVisitor(Ctx, Param);
      RoleVisitor.TraverseStmt(const_cast<Stmt *>(FD->getBody()));
      if (RoleVisitor.hasSectorCapacityRole())
        reportParam(BR, Param);
    }

    LocalTruncationVisitor LocalVisitor(Ctx, BR, this, *BugTy);
    LocalVisitor.TraverseStmt(const_cast<Stmt *>(FD->getBody()));
  }

private:
  void reportParam(BugReporter &BR, const ParmVarDecl *Param) const {
    PathDiagnosticLocation Loc = PathDiagnosticLocation::createBegin(Param, BR.getSourceManager());
    auto Report = std::make_unique<BasicBugReport>(
        *BugTy,
        "32-bit sector count is bounded with a wider capacity and then used for reservation/accounting",
        Loc);
    Report->addRange(Param->getSourceRange());
    BR.emitReport(std::move(Report));
  }
};

} // namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects 32-bit truncation of sector/capacity quantities before accounting sinks",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
