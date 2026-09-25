// Detect patch-local sector count truncation fixed by widening to u64.
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
#include <memory>

using namespace clang;
using namespace ento;

namespace {

static bool isUnsigned32(QualType QT, ASTContext &Ctx) {
  QT = QT.getCanonicalType();
  return QT->isUnsignedIntegerType() && Ctx.getTypeSize(QT) <= 32;
}

class SectorWidthVisitor : public RecursiveASTVisitor<SectorWidthVisitor> {
  ASTContext &Ctx;
  BugReporter &BR;
  BugType &BugTy;
  llvm::StringRef FunctionName;

public:
  SectorWidthVisitor(ASTContext &Ctx, BugReporter &BR, BugType &BugTy,
                     llvm::StringRef FunctionName)
      : Ctx(Ctx), BR(BR), BugTy(BugTy), FunctionName(FunctionName) {}

  bool VisitVarDecl(VarDecl *VD) {
    if (FunctionName != "bch2_trans_fs_usage_apply")
      return true;
    if (!VD->getIdentifier() || VD->getName() != "disk_res_sectors")
      return true;
    if (!isUnsigned32(VD->getType(), Ctx))
      return true;

    emit(VD, "sector reservation count is stored in 32-bit unsigned before usage accounting");
    return true;
  }

private:
  void emit(const VarDecl *VD, llvm::StringRef Message) {
    PathDiagnosticLocation Loc =
        PathDiagnosticLocation::createBegin(VD, BR.getSourceManager());
    auto Report = std::make_unique<BasicBugReport>(BugTy, Message, Loc);
    Report->addRange(VD->getSourceRange());
    BR.emitReport(std::move(Report));
  }
};

class SAGenTestChecker : public Checker<check::ASTCodeBody> {
  mutable std::unique_ptr<BugType> BugTy;

public:
  SAGenTestChecker()
      : BugTy(std::make_unique<BugType>(
            this, "32-bit sector count truncation", "Integer Overflow")) {}

  void checkASTCodeBody(const Decl *D, AnalysisManager &, BugReporter &BR) const {
    const auto *FD = dyn_cast<FunctionDecl>(D);
    if (!FD || !FD->hasBody())
      return;

    ASTContext &Ctx = BR.getContext();
    llvm::StringRef Name = FD->getName();

    if (Name == "bch2_extent_fallocate") {
      for (const ParmVarDecl *Param : FD->parameters()) {
        if (!Param->getIdentifier() || Param->getName() != "sectors")
          continue;
        if (!isUnsigned32(Param->getType(), Ctx))
          continue;
        reportParam(BR, Param);
      }
    }

    SectorWidthVisitor Visitor(Ctx, BR, *BugTy, Name);
    Visitor.TraverseStmt(const_cast<Stmt *>(FD->getBody()));
  }

private:
  void reportParam(BugReporter &BR, const ParmVarDecl *Param) const {
    PathDiagnosticLocation Loc =
        PathDiagnosticLocation::createBegin(Param, BR.getSourceManager());
    auto Report = std::make_unique<BasicBugReport>(
        *BugTy,
        "fallocate sector count parameter is 32-bit unsigned before u64 accounting",
        Loc);
    Report->addRange(Param->getSourceRange());
    BR.emitReport(std::move(Report));
  }
};

} // namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects patch-local 32-bit sector count truncation before u64 accounting",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
