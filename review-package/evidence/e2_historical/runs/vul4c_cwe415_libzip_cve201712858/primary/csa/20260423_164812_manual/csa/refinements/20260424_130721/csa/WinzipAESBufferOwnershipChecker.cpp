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

static bool isZipBufferFreeOnBuffer(const CallExpr *CE) {
  if (!CE || CE->getNumArgs() < 1)
    return false;
  return isFunctionNamed(CE->getDirectCallee(), "_zip_buffer_free") &&
         isVarNamed(CE->getArg(0), "buffer");
}

static bool isWinzipAesFailureCheck(const Expr *Cond) {
  Cond = ignoreParenCasts(Cond);
  const auto *UO = dyn_cast_or_null<UnaryOperator>(Cond);
  if (!UO || UO->getOpcode() != UO_LNot)
    return false;
  const auto *CE = dyn_cast_or_null<CallExpr>(ignoreParenCasts(UO->getSubExpr()));
  return CE && isFunctionNamed(CE->getDirectCallee(), "_zip_dirent_process_winzip_aes");
}

static bool isFailureReturn(const ReturnStmt *RS) {
  const Expr *Ret = ignoreParenCasts(RS ? RS->getRetValue() : nullptr);
  const auto *UO = dyn_cast_or_null<UnaryOperator>(Ret);
  const auto *IL = UO && UO->getOpcode() == UO_Minus
                       ? dyn_cast_or_null<IntegerLiteral>(ignoreParenCasts(UO->getSubExpr()))
                       : nullptr;
  return IL && IL->getValue() == 1;
}

class WinzipAESBufferOwnershipChecker : public Checker<check::ASTCodeBody> {
  mutable std::unique_ptr<BugType> BT;

public:
  WinzipAESBufferOwnershipChecker()
      : BT(std::make_unique<BugType>(
            this, "WinZip AES failure path frees owned buffer before later cleanup",
            "Memory error")) {}

  void checkASTCodeBody(const Decl *D, AnalysisManager &, BugReporter &BR) const {
    const auto *FD = dyn_cast_or_null<FunctionDecl>(D);
    if (!FD || !FD->hasBody())
      return;
    if (!isFunctionNamed(FD, "_zip_dirent_read") || !inTargetFile(FD))
      return;

    inspectForWinzipFailure(FD->getBody(), FD, BR);
  }

private:
  void inspectForWinzipFailure(const Stmt *S, const FunctionDecl *FD,
                               BugReporter &BR) const {
    if (!S)
      return;

    if (const auto *IfS = dyn_cast<IfStmt>(S)) {
      if (isWinzipAesFailureCheck(IfS->getCond()) &&
          containsFailureReturn(IfS->getThen())) {
        if (const CallExpr *Free = findGuardedBufferFree(IfS->getThen(), false)) {
          emitBug(Free, FD, BR);
          return;
        }
      }
    }

    for (const Stmt *Child : S->children())
      inspectForWinzipFailure(Child, FD, BR);
  }

  const CallExpr *findGuardedBufferFree(const Stmt *S,
                                        bool UnderFromBufferGuard) const {
    if (!S)
      return nullptr;

    if (const auto *IfS = dyn_cast<IfStmt>(S)) {
      const bool Guarded = UnderFromBufferGuard || isNegatedFromBuffer(IfS->getCond());
      if (const CallExpr *Found = findGuardedBufferFree(IfS->getThen(), Guarded))
        return Found;
      if (const CallExpr *Found = findGuardedBufferFree(IfS->getElse(), UnderFromBufferGuard))
        return Found;
      return nullptr;
    }

    if (const auto *CE = dyn_cast<CallExpr>(S)) {
      if (UnderFromBufferGuard && isZipBufferFreeOnBuffer(CE))
        return CE;
    }

    for (const Stmt *Child : S->children()) {
      if (const CallExpr *Found = findGuardedBufferFree(Child, UnderFromBufferGuard))
        return Found;
    }
    return nullptr;
  }

  bool containsFailureReturn(const Stmt *S) const {
    if (!S)
      return false;
    if (const auto *RS = dyn_cast<ReturnStmt>(S))
      return isFailureReturn(RS);
    for (const Stmt *Child : S->children()) {
      if (containsFailureReturn(Child))
        return true;
    }
    return false;
  }

  void emitBug(const CallExpr *FreeCall, const FunctionDecl *FD,
               BugReporter &BR) const {
    PathDiagnosticLocation Loc(FreeCall->getExprLoc(), BR.getSourceManager());
    SmallVector<SourceRange, 1> Ranges;
    Ranges.push_back(FreeCall->getSourceRange());

    BR.EmitBasicReport(
        FD, this,
        "WinZip AES failure path frees buffer before ownership is invalidated",
        "Memory error",
        "When _zip_dirent_process_winzip_aes fails, this path releases the local "
        "buffer under !from_buffer and then returns, leaving the same cleanup-owned "
        "resource model inconsistent with the patched version that removes this "
        "early release.",
        Loc, Ranges);
  }
};

} // namespace

extern "C" void clang_registerCheckers(CheckerRegistry &Registry) {
  Registry.addChecker<WinzipAESBufferOwnershipChecker>(
      "custom.WinzipAESBufferOwnershipChecker",
      "Detects the libzip WinZip AES failure-path buffer release removed by CVE-2017-12858.",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] = CLANG_VERSION_STRING;
