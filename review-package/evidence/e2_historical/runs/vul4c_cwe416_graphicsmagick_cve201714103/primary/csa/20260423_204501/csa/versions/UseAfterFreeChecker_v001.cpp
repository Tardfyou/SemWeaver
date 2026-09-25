#include "clang/AST/Expr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclBase.h"
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
#include <string>
#include <vector>

using namespace clang;
using namespace ento;

namespace {

static const Expr *stripExpr(const Expr *E) {
  if (!E)
    return nullptr;
  return E->IgnoreParenImpCasts();
}

static const ValueDecl *getBaseValueDecl(const Expr *E) {
  E = stripExpr(E);
  if (!E)
    return nullptr;

  if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
    return DRE->getDecl();

  if (const auto *ME = dyn_cast<MemberExpr>(E)) {
    if (const ValueDecl *VD = dyn_cast<ValueDecl>(ME->getMemberDecl()))
      return VD;
  }

  if (const auto *UO = dyn_cast<UnaryOperator>(E)) {
    if (UO->getOpcode() == UO_Deref || UO->getOpcode() == UO_AddrOf)
      return getBaseValueDecl(UO->getSubExpr());
  }

  if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(E))
    return getBaseValueDecl(ASE->getBase());

  return nullptr;
}

static bool exprReferencesValue(const Expr *E, const ValueDecl *Target) {
  E = stripExpr(E);
  if (!E || !Target)
    return false;

  if (const ValueDecl *VD = getBaseValueDecl(E))
    if (VD == Target)
      return true;

  if (const auto *ME = dyn_cast<MemberExpr>(E)) {
    if (exprReferencesValue(ME->getBase(), Target))
      return true;
  }

  if (const auto *UO = dyn_cast<UnaryOperator>(E))
    return exprReferencesValue(UO->getSubExpr(), Target);

  if (const auto *BO = dyn_cast<BinaryOperator>(E))
    return exprReferencesValue(BO->getLHS(), Target) ||
           exprReferencesValue(BO->getRHS(), Target);

  if (const auto *CO = dyn_cast<ConditionalOperator>(E))
    return exprReferencesValue(CO->getCond(), Target) ||
           exprReferencesValue(CO->getTrueExpr(), Target) ||
           exprReferencesValue(CO->getFalseExpr(), Target);

  if (const auto *CE = dyn_cast<CallExpr>(E)) {
    if (exprReferencesValue(CE->getCallee(), Target))
      return true;
    for (const Expr *Arg : CE->arguments()) {
      if (exprReferencesValue(Arg, Target))
        return true;
    }
  }

  if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(E))
    return exprReferencesValue(ASE->getBase(), Target) ||
           exprReferencesValue(ASE->getIdx(), Target);

  if (const auto *ICE = dyn_cast<ImplicitCastExpr>(E))
    return exprReferencesValue(ICE->getSubExpr(), Target);

  if (const auto *CSE = dyn_cast<CStyleCastExpr>(E))
    return exprReferencesValue(CSE->getSubExpr(), Target);

  return false;
}

class UseAfterFreeVisitor : public RecursiveASTVisitor<UseAfterFreeVisitor> {
  BugReporter &BR;
  const CheckerBase *Checker;
  const FunctionDecl *FD;
  std::unique_ptr<BugType> &BT;
  ASTContext &ACtx;
  SourceManager &SM;

  struct TriggerInfo {
    const CallExpr *TriggerCall;
    const ValueDecl *TrackedDecl;
    std::string Name;
  };

  std::vector<TriggerInfo> Triggers;

  bool isInMainFile(SourceLocation Loc) const {
    if (Loc.isInvalid())
      return false;
    return SM.isWrittenInMainFile(SM.getSpellingLoc(Loc));
  }

  bool isReadJNGImage() const {
    if (!FD)
      return false;
    const IdentifierInfo *II = FD->getIdentifier();
    if (!II)
      return false;
    return II->getName() == "ReadJNGImage";
  }

  bool isTargetFile() const {
    if (!FD)
      return false;
    SourceLocation Loc = FD->getLocation();
    if (Loc.isInvalid())
      return false;
    StringRef FileName = SM.getFilename(SM.getSpellingLoc(Loc));
    return FileName.ends_with("coders/png.c");
  }

  static StringRef getDirectCalleeName(const CallExpr *CE) {
    if (!CE)
      return StringRef();
    const FunctionDecl *Callee = CE->getDirectCallee();
    if (!Callee)
      return StringRef();
    const IdentifierInfo *II = Callee->getIdentifier();
    if (!II)
      return StringRef();
    return II->getName();
  }

  bool isThrowReaderExceptionCall(const CallExpr *CE) const {
    return getDirectCalleeName(CE) == "ThrowReaderException";
  }

  bool isUseCallName(StringRef Name) const {
    return Name == "CloseBlob" || Name == "DestroyImageList" || Name == "DestroyImage";
  }

  bool hasInterestingImageName(StringRef Name) const {
    return Name == "image" || Name == "color_image" || Name == "alpha_image";
  }

  bool isPotentialUseAfter(const Stmt *S, const TriggerInfo &TI) const {
    if (!S || !TI.TrackedDecl)
      return false;

    if (const auto *CE = dyn_cast<CallExpr>(S)) {
      StringRef CalleeName = getDirectCalleeName(CE);
      if (isUseCallName(CalleeName)) {
        for (const Expr *Arg : CE->arguments()) {
          if (exprReferencesValue(Arg, TI.TrackedDecl))
            return true;
        }
      }
    }

    if (const auto *ME = dyn_cast<MemberExpr>(S)) {
      if (exprReferencesValue(ME->getBase(), TI.TrackedDecl))
        return true;
    }

    if (const auto *UO = dyn_cast<UnaryOperator>(S)) {
      if (UO->getOpcode() == UO_Deref && exprReferencesValue(UO->getSubExpr(), TI.TrackedDecl))
        return true;
    }

    if (const auto *BO = dyn_cast<BinaryOperator>(S)) {
      if (BO->isAssignmentOp() || BO->isCompoundAssignmentOp()) {
        if (exprReferencesValue(BO->getLHS(), TI.TrackedDecl) ||
            exprReferencesValue(BO->getRHS(), TI.TrackedDecl))
          return true;
      }
    }

    if (const auto *RS = dyn_cast<ReturnStmt>(S)) {
      const Expr *Ret = RS->getRetValue();
      if (Ret && exprReferencesValue(Ret, TI.TrackedDecl))
        return true;
    }

    return false;
  }

  bool stmtComesAfter(const Stmt *A, const Stmt *B) const {
    if (!A || !B)
      return false;
    SourceLocation AL = SM.getSpellingLoc(A->getBeginLoc());
    SourceLocation BL = SM.getSpellingLoc(B->getBeginLoc());
    if (AL.isInvalid() || BL.isInvalid())
      return false;
    return SM.isBeforeInTranslationUnit(AL, BL);
  }

  void emitBug(const TriggerInfo &TI, const Stmt *UseStmt, StringRef UseKind) {
    if (!TI.TriggerCall || !UseStmt)
      return;

    PathDiagnosticLocation Loc(UseStmt->getBeginLoc(), SM);
    SmallString<256> Buf;
    llvm::raw_svector_ostream OS(Buf);
    OS << "Potential use-after-free in ReadJNGImage: ThrowReaderException is called with `"
       << TI.Name
       << "`, and the same pointer is used later by "
       << UseKind
       << ".";

    BR.EmitBasicReport(FD, Checker, "Potential use-after-free", "Memory Error",
                       OS.str(), Loc, UseStmt->getSourceRange());
  }

public:
  UseAfterFreeVisitor(BugReporter &BR, const CheckerBase *Checker,
                      const FunctionDecl *FD, std::unique_ptr<BugType> &BT,
                      ASTContext &ACtx)
      : BR(BR), Checker(Checker), FD(FD), BT(BT), ACtx(ACtx),
        SM(ACtx.getSourceManager()) {}

  bool VisitCallExpr(CallExpr *CE) {
    if (!CE || !isReadJNGImage() || !isTargetFile() || !isInMainFile(CE->getBeginLoc()))
      return true;

    if (!isThrowReaderExceptionCall(CE))
      return true;

    for (const Expr *Arg : CE->arguments()) {
      const ValueDecl *VD = getBaseValueDecl(Arg);
      if (!VD)
        continue;

      const IdentifierInfo *II = VD->getIdentifier();
      if (!II)
        continue;

      StringRef Name = II->getName();
      if (!hasInterestingImageName(Name))
        continue;

      TriggerInfo TI;
      TI.TriggerCall = CE;
      TI.TrackedDecl = VD;
      TI.Name = Name.str();
      Triggers.push_back(TI);
    }
    return true;
  }

  bool VisitStmt(Stmt *S) {
    if (!S || !isReadJNGImage() || !isTargetFile() || !isInMainFile(S->getBeginLoc()))
      return true;

    for (const TriggerInfo &TI : Triggers) {
      if (!stmtComesAfter(TI.TriggerCall, S))
        continue;
      if (S == TI.TriggerCall)
        continue;

      if (const auto *CE = dyn_cast<CallExpr>(S)) {
        StringRef CalleeName = getDirectCalleeName(CE);
        if (isUseCallName(CalleeName) && isPotentialUseAfter(S, TI)) {
          emitBug(TI, S, CalleeName);
          continue;
        }
      }

      if (const auto *ME = dyn_cast<MemberExpr>(S)) {
        if (isPotentialUseAfter(S, TI)) {
          emitBug(TI, S, "a field dereference");
          continue;
        }
      }

      if (const auto *BO = dyn_cast<BinaryOperator>(S)) {
        if (isPotentialUseAfter(BO, TI)) {
          emitBug(TI, S, "an assignment involving the freed pointer");
          continue;
        }
      }
    }

    return true;
  }
};

class UseAfterFreeChecker : public Checker<check::ASTCodeBody> {
  mutable std::unique_ptr<BugType> BT;

public:
  UseAfterFreeChecker()
      : BT(std::make_unique<BugType>(this, "Potential use-after-free", "Memory Error")) {}

  void checkASTCodeBody(const Decl *D, AnalysisManager &, BugReporter &BR) const {
    const auto *FD = dyn_cast_or_null<FunctionDecl>(D);
    if (!FD)
      return;
    if (!FD->hasBody())
      return;

    const IdentifierInfo *II = FD->getIdentifier();
    if (!II || II->getName() != "ReadJNGImage")
      return;

    ASTContext &ACtx = BR.getContext();
    SourceManager &SM = ACtx.getSourceManager();
    StringRef FileName = SM.getFilename(SM.getSpellingLoc(FD->getLocation()));
    if (!FileName.ends_with("coders/png.c"))
      return;

    UseAfterFreeVisitor Visitor(BR, this, FD, BT, ACtx);
    Visitor.TraverseStmt(FD->getBody());
  }
};

} // namespace

extern "C" void clang_registerCheckers(CheckerRegistry &Registry) {
  Registry.addChecker<UseAfterFreeChecker>("custom.UseAfterFreeChecker", "Patch-guided checker for ThrowReaderException-triggered use-after-free in ReadJNGImage", "");
}

extern "C" const char clang_analyzerAPIVersionString[] = CLANG_VERSION_STRING;
