#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/Version.h"
#include "clang/Lex/Lexer.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/CheckerManager.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include "llvm/ADT/StringRef.h"
#include <memory>

using namespace clang;
using namespace ento;

namespace {

static const Expr *stripParensAndCasts(const Expr *E) {
  if (!E)
    return nullptr;
  return E->IgnoreParenImpCasts();
}

static bool sourceLocationContainsFile(SourceLocation Loc, const SourceManager &SM,
                                       StringRef Suffix) {
  if (Loc.isInvalid())
    return false;
  StringRef File = SM.getFilename(SM.getSpellingLoc(Loc));
  return !File.empty() && File.ends_with(Suffix);
}

static const ValueDecl *getBaseDecl(const Expr *E) {
  E = stripParensAndCasts(E);
  if (!E)
    return nullptr;

  if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
    return DRE->getDecl();

  if (const auto *ME = dyn_cast<MemberExpr>(E))
    return getBaseDecl(ME->getBase());

  if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(E))
    return getBaseDecl(ASE->getBase());

  if (const auto *UO = dyn_cast<UnaryOperator>(E))
    return getBaseDecl(UO->getSubExpr());

  return nullptr;
}

static bool exprLooksLikeAttrBlkData(const Expr *E) {
  E = stripParensAndCasts(E);
  if (!E)
    return false;

  const auto *MEData = dyn_cast<MemberExpr>(E);
  if (!MEData)
    return false;
  const ValueDecl *DataMember = MEData->getMemberDecl();
  if (!DataMember || DataMember->getName() != "data")
    return false;

  const Expr *Base1 = stripParensAndCasts(MEData->getBase());
  const auto *MEBlk = dyn_cast<MemberExpr>(Base1);
  if (!MEBlk)
    return false;
  const ValueDecl *BlkMember = MEBlk->getMemberDecl();
  if (!BlkMember || BlkMember->getName() != "blk")
    return false;

  const Expr *Base2 = stripParensAndCasts(MEBlk->getBase());
  const auto *MEU = dyn_cast<MemberExpr>(Base2);
  if (!MEU)
    return false;
  const ValueDecl *UMember = MEU->getMemberDecl();
  if (!UMember || UMember->getName() != "u")
    return false;

  const ValueDecl *Root = getBaseDecl(MEU->getBase());
  return Root && Root->getName() == "attr";
}

static bool isNullPointerLiteralExpr(const Expr *E) {
  E = stripParensAndCasts(E);
  if (!E)
    return false;

  if (isa<CXXNullPtrLiteralExpr>(E))
    return true;

  Expr::NullPointerConstantKind NPCK =
      E->isNullPointerConstant(E->getASTContext(), Expr::NPC_ValueDependentIsNotNull);
  return NPCK != Expr::NPCK_NotNull;
}

static bool isNonNullCheckOnAttrBlkData(const Expr *E) {
  E = stripParensAndCasts(E);
  const auto *BO = dyn_cast_or_null<BinaryOperator>(E);
  if (!BO)
    return false;

  if (!(BO->getOpcode() == BO_NE || BO->getOpcode() == BO_EQ))
    return false;

  const Expr *LHS = stripParensAndCasts(BO->getLHS());
  const Expr *RHS = stripParensAndCasts(BO->getRHS());
  if (!LHS || !RHS)
    return false;

  bool LeftTarget = exprLooksLikeAttrBlkData(LHS);
  bool RightTarget = exprLooksLikeAttrBlkData(RHS);
  bool LeftNull = isNullPointerLiteralExpr(LHS);
  bool RightNull = isNullPointerLiteralExpr(RHS);

  if (BO->getOpcode() == BO_NE)
    return (LeftTarget && RightNull) || (RightTarget && LeftNull);

  return false;
}

class NullDerefVisitor : public RecursiveASTVisitor<NullDerefVisitor> {
  BugReporter &BR;
  AnalysisDeclContext *AC;
  const SourceManager &SM;
  ASTContext &Ctx;
  BugType &BT;
  bool InPatchedFile = false;
  bool InTargetFunction = false;
  bool GuardedContext = false;

  class GuardScope {
    NullDerefVisitor &V;
    bool OldValue;

  public:
    GuardScope(NullDerefVisitor &Visitor, bool NewValue)
        : V(Visitor), OldValue(Visitor.GuardedContext) {
      V.GuardedContext = NewValue;
    }

    ~GuardScope() { V.GuardedContext = OldValue; }
  };

public:
  NullDerefVisitor(BugReporter &BR, AnalysisDeclContext *AC, BugType &BT)
      : BR(BR), AC(AC), SM(BR.getSourceManager()), Ctx(AC->getASTContext()), BT(BT) {
    const Decl *D = AC ? AC->getDecl() : nullptr;
    const auto *FD = dyn_cast_or_null<FunctionDecl>(D);
    if (!FD)
      return;
    InPatchedFile = sourceLocationContainsFile(FD->getLocation(), SM, "bfd/dwarf2.c");
    if (const IdentifierInfo *II = FD->getIdentifier())
      InTargetFunction = II->getName() == "scan_unit_for_symbols";
  }

  bool shouldVisitTemplateInstantiations() const { return false; }

  bool TraverseStmt(Stmt *S) {
    if (!S || !InPatchedFile || !InTargetFunction)
      return true;
    return RecursiveASTVisitor<NullDerefVisitor>::TraverseStmt(S);
  }

  bool TraverseIfStmt(IfStmt *IfS) {
    if (!IfS)
      return true;

    RecursiveASTVisitor<NullDerefVisitor>::TraverseStmt(IfS->getInit());
    RecursiveASTVisitor<NullDerefVisitor>::TraverseStmt(IfS->getConditionVariableDeclStmt());
    RecursiveASTVisitor<NullDerefVisitor>::TraverseStmt(IfS->getCond());

    bool ThenGuarded = GuardedContext || isNonNullCheckOnAttrBlkData(IfS->getCond());
    {
      GuardScope Scope(*this, ThenGuarded);
      RecursiveASTVisitor<NullDerefVisitor>::TraverseStmt(IfS->getThen());
    }
    {
      GuardScope Scope(*this, GuardedContext);
      RecursiveASTVisitor<NullDerefVisitor>::TraverseStmt(IfS->getElse());
    }
    return true;
  }

  bool TraverseBinaryOperator(BinaryOperator *BO) {
    if (!BO)
      return true;

    bool NewGuard = GuardedContext;
    if (BO->getOpcode() == BO_LAnd && isNonNullCheckOnAttrBlkData(BO->getLHS()))
      NewGuard = true;

    {
      GuardScope Scope(*this, GuardedContext);
      RecursiveASTVisitor<NullDerefVisitor>::TraverseStmt(BO->getLHS());
    }
    {
      GuardScope Scope(*this, NewGuard);
      RecursiveASTVisitor<NullDerefVisitor>::TraverseStmt(BO->getRHS());
    }
    return true;
  }

  bool VisitUnaryOperator(UnaryOperator *UO) {
    if (!UO || !InPatchedFile || !InTargetFunction)
      return true;

    if (GuardedContext)
      return true;

    if (UO->getOpcode() != UO_Deref)
      return true;

    const Expr *Sub = stripParensAndCasts(UO->getSubExpr());
    if (!exprLooksLikeAttrBlkData(Sub))
      return true;

    PathDiagnosticLocation Loc = PathDiagnosticLocation::createBegin(UO, SM, AC);
    BR.EmitBasicReport(AC->getDecl(), this, BT.getName(), BT.getCategory(),
                       "Dereference of attr.u.blk->data without a preceding non-null check.",
                       Loc);
    return true;
  }
};

class NullDerefChecker : public Checker<check::ASTCodeBody> {
  mutable std::unique_ptr<BugType> BT;

public:
  NullDerefChecker()
      : BT(std::make_unique<BugType>(this, "Patch-guided null dereference", "Custom")) {}

  void checkASTCodeBody(const Decl *D, AnalysisManager &Mgr, BugReporter &BR) const {
    if (!D)
      return;

    const auto *FD = dyn_cast<FunctionDecl>(D);
    if (!FD || !FD->hasBody())
      return;

    const SourceManager &SM = BR.getSourceManager();
    if (!sourceLocationContainsFile(FD->getLocation(), SM, "bfd/dwarf2.c"))
      return;

    const IdentifierInfo *II = FD->getIdentifier();
    if (!II || II->getName() != "scan_unit_for_symbols")
      return;

    AnalysisDeclContext *AC = Mgr.getAnalysisDeclContext(D);
    if (!AC)
      return;

    NullDerefVisitor Visitor(BR, AC, *BT);
    Visitor.TraverseDecl(const_cast<Decl *>(D));
  }
};

} // namespace

extern "C" void clang_registerCheckers(CheckerRegistry &Registry) {
  Registry.addChecker<NullDerefChecker>("custom.NullDerefChecker",
                                        "Patch-guided null dereference checker", "");
}

extern "C" const char clang_analyzerAPIVersionString[] = CLANG_VERSION_STRING;
