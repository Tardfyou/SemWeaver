#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "clang/Basic/Version.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/CheckerManager.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include <memory>

using namespace clang;
using namespace ento;

namespace {

static const Expr *ignoreParenCasts(const Expr *E) {
  return E ? E->IgnoreParenCasts() : nullptr;
}

static const Expr *stripAddressOf(const Expr *E) {
  E = ignoreParenCasts(E);
  if (const auto *UO = dyn_cast_or_null<UnaryOperator>(E)) {
    if (UO->getOpcode() == UO_AddrOf)
      return ignoreParenCasts(UO->getSubExpr());
  }
  return E;
}

static const DeclRefExpr *asDeclRef(const Expr *E) {
  return dyn_cast_or_null<DeclRefExpr>(stripAddressOf(E));
}

static bool isVarNamed(const Expr *E, llvm::StringRef Name) {
  const auto *DRE = asDeclRef(E);
  if (!DRE)
    return false;
  const auto *VD = dyn_cast<VarDecl>(DRE->getDecl());
  return VD && VD->getName() == Name;
}

static bool isFunctionNamed(const FunctionDecl *FD, llvm::StringRef Name) {
  return FD && FD->getIdentifier() && FD->getIdentifier()->getName() == Name;
}

static bool inTargetFile(const FunctionDecl *FD) {
  if (!FD)
    return false;
  const SourceManager &SM = FD->getASTContext().getSourceManager();
  StringRef Path = SM.getFilename(FD->getLocation());
  return Path.ends_with("lib/zip_dirent.c") || Path.ends_with("\\lib\\zip_dirent.c");
}

static bool isNegatedFromBuffer(const Expr *Cond) {
  Cond = ignoreParenCasts(Cond);
  const auto *UO = dyn_cast_or_null<UnaryOperator>(Cond);
  return UO && UO->getOpcode() == UO_LNot &&
         isVarNamed(UO->getSubExpr(), "from_buffer");
}

static bool isFromBufferGuard(const Stmt *S) {
  const auto *IfS = dyn_cast_or_null<IfStmt>(S);
  return IfS && isNegatedFromBuffer(IfS->getCond());
}

static bool isZipBufferFreeOnBuffer(const CallExpr *CE) {
  if (!CE || CE->getNumArgs() < 1)
    return false;

  const FunctionDecl *Callee = CE->getDirectCallee();
  if (!isFunctionNamed(Callee, "_zip_buffer_free"))
    return false;

  return isVarNamed(CE->getArg(0), "buffer");
}

static bool isBorrowedBufferContext(const FunctionDecl *FD) {
  if (!FD || !FD->hasBody())
    return false;

  const auto *Body = FD->getBody();
  for (const Stmt *Child : Body->children()) {
    if (isFromBufferGuard(Child))
      return true;
  }
  return false;
}

class WinzipAESBufferOwnershipChecker : public Checker<check::ASTCodeBody> {
  mutable std::unique_ptr<BugType> BT;

public:
  WinzipAESBufferOwnershipChecker()
      : BT(std::make_unique<BugType>(
            this, "Borrowed buffer freed on multiple exits", "Memory error")) {}

  void checkASTCodeBody(const Decl *D, AnalysisManager &, BugReporter &BR) const {
    const auto *FD = dyn_cast_or_null<FunctionDecl>(D);
    if (!FD || !FD->hasBody())
      return;
    if (!isFunctionNamed(FD, "_zip_dirent_read"))
      return;
    if (!inTargetFile(FD))
      return;
    if (!isBorrowedBufferContext(FD))
      return;

    inspectStmt(FD->getBody(), FD, BR, false, 0);
  }

private:
  void inspectStmt(const Stmt *S, const FunctionDecl *FD, BugReporter &BR,
                   bool UnderFromBufferGuard, unsigned PriorGuardedFrees) const {
    if (!S)
      return;

    if (const auto *IfS = dyn_cast<IfStmt>(S)) {
      const bool IsGuard = isNegatedFromBuffer(IfS->getCond());
      const bool GuardedContext = UnderFromBufferGuard || IsGuard;
      const unsigned ThenFrees = countGuardedBufferFrees(IfS->getThen(), GuardedContext);
      const unsigned TotalThenFrees = PriorGuardedFrees + ThenFrees;

      if (IsGuard && PriorGuardedFrees > 0)
        reportDuplicateGuardedFrees(IfS->getThen(), FD, BR);

      inspectStmt(IfS->getThen(), FD, BR, GuardedContext, PriorGuardedFrees);
      inspectStmt(IfS->getElse(), FD, BR, UnderFromBufferGuard, TotalThenFrees);
      return;
    }

    unsigned CurrentGuardedFrees = PriorGuardedFrees;
    if (const auto *CE = dyn_cast<CallExpr>(S)) {
      if (UnderFromBufferGuard && isZipBufferFreeOnBuffer(CE)) {
        if (PriorGuardedFrees > 0)
          emitBug(CE, FD, BR);
        ++CurrentGuardedFrees;
      }
    }

    for (const Stmt *Child : S->children())
      inspectStmt(Child, FD, BR, UnderFromBufferGuard, CurrentGuardedFrees);
  }

  unsigned countGuardedBufferFrees(const Stmt *S, bool UnderFromBufferGuard) const {
    if (!S)
      return 0;

    unsigned Count = 0;
    if (const auto *IfS = dyn_cast<IfStmt>(S)) {
      const bool GuardedContext = UnderFromBufferGuard || isNegatedFromBuffer(IfS->getCond());
      Count += countGuardedBufferFrees(IfS->getThen(), GuardedContext);
      Count += countGuardedBufferFrees(IfS->getElse(), UnderFromBufferGuard);
      return Count;
    }

    if (const auto *CE = dyn_cast<CallExpr>(S)) {
      if (UnderFromBufferGuard && isZipBufferFreeOnBuffer(CE))
        ++Count;
    }

    for (const Stmt *Child : S->children())
      Count += countGuardedBufferFrees(Child, UnderFromBufferGuard);
    return Count;
  }

  void reportDuplicateGuardedFrees(const Stmt *S, const FunctionDecl *FD,
                                   BugReporter &BR) const {
    if (!S)
      return;

    if (const auto *CE = dyn_cast<CallExpr>(S)) {
      if (isZipBufferFreeOnBuffer(CE))
        emitBug(CE, FD, BR);
    }

    for (const Stmt *Child : S->children())
      reportDuplicateGuardedFrees(Child, FD, BR);
  }

  void emitBug(const CallExpr *FreeCall, const FunctionDecl *FD,
               BugReporter &BR) const {
    PathDiagnosticLocation Loc(FreeCall->getExprLoc(), BR.getSourceManager());
    SmallVector<SourceRange, 1> Ranges;
    Ranges.push_back(FreeCall->getSourceRange());

    BR.EmitBasicReport(
        FD, this, "Borrowed buffer may be freed repeatedly on error paths",
        "Memory error",
        "A buffer that is only freed when !from_buffer should have a single guarded "
        "ownership-release point. Repeating _zip_buffer_free(buffer) on multiple "
        "guarded exits can leave later cleanup paths operating on an already released "
        "borrowed buffer.",
        Loc, Ranges);
  }
};

} // namespace

extern "C" void clang_registerCheckers(CheckerRegistry &Registry) {
  Registry.addChecker<WinzipAESBufferOwnershipChecker>(
      "custom.WinzipAESBufferOwnershipChecker",
      "Detects repeated guarded releases of a borrowed zip buffer across multiple exits.",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] = CLANG_VERSION_STRING;
