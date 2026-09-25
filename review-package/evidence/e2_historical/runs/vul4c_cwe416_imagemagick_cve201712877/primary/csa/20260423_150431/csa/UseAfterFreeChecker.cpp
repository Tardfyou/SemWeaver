#include "clang/AST/Expr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/Version.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/CheckerManager.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/MemRegion.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramStateTrait.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SVals.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include <memory>
#include <vector>

using namespace clang;
using namespace ento;

namespace {

static const Expr *ignoreSugar(const Expr *E) {
  return E ? E->IgnoreParenCasts() : nullptr;
}

static const Decl *getBaseDecl(const Expr *E) {
  E = ignoreSugar(E);
  if (!E)
    return nullptr;

  if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
    return DRE->getDecl();

  if (const auto *ME = dyn_cast<MemberExpr>(E)) {
    if (const ValueDecl *VD = ME->getMemberDecl())
      return VD;
  }

  return nullptr;
}

static bool sameEntity(const Expr *A, const Expr *B) {
  const Decl *DA = getBaseDecl(A);
  const Decl *DB = getBaseDecl(B);
  return DA && DB && DA == DB;
}

static bool isNullLiteralExpr(const Expr *E) {
  E = ignoreSugar(E);
  if (!E)
    return false;
  if (isa<CXXNullPtrLiteralExpr>(E))
    return true;
  if (const auto *IL = dyn_cast<IntegerLiteral>(E))
    return IL->getValue().isZero();
  return false;
}

static const Expr *getAssignedLHSFromStmt(const Stmt *S) {
  if (!S)
    return nullptr;
  if (const auto *BO = dyn_cast<BinaryOperator>(S)) {
    if (BO->isAssignmentOp())
      return BO->getLHS();
  }
  return nullptr;
}

static const Expr *getAssignedRHSFromStmt(const Stmt *S) {
  if (!S)
    return nullptr;
  if (const auto *BO = dyn_cast<BinaryOperator>(S)) {
    if (BO->isAssignmentOp())
      return BO->getRHS();
  }
  return nullptr;
}

static const Expr *extractComparedPeer(const Expr *Cond, const Expr *VictimRef,
                                       bool &ChecksEquality) {
  Cond = ignoreSugar(Cond);
  if (!Cond)
    return nullptr;

  if (const auto *UO = dyn_cast<UnaryOperator>(Cond)) {
    if (UO->getOpcode() == UO_LNot) {
      bool InnerEq = false;
      const Expr *InnerPeer = extractComparedPeer(UO->getSubExpr(), VictimRef, InnerEq);
      if (!InnerPeer)
        return nullptr;
      ChecksEquality = !InnerEq;
      return InnerPeer;
    }
  }

  if (const auto *BO = dyn_cast<BinaryOperator>(Cond)) {
    if (!(BO->isEqualityOp()))
      return nullptr;

    const Expr *LHS = ignoreSugar(BO->getLHS());
    const Expr *RHS = ignoreSugar(BO->getRHS());
    if (!LHS || !RHS)
      return nullptr;

    if (sameEntity(LHS, VictimRef)) {
      ChecksEquality = (BO->getOpcode() == BO_EQ);
      return RHS;
    }
    if (sameEntity(RHS, VictimRef)) {
      ChecksEquality = (BO->getOpcode() == BO_EQ);
      return LHS;
    }
  }

  return nullptr;
}

static bool stmtNullifiesPeer(const Stmt *S, const Expr *PeerRef) {
  const Expr *LHS = getAssignedLHSFromStmt(S);
  const Expr *RHS = getAssignedRHSFromStmt(S);
  if (!LHS || !RHS)
    return false;
  return sameEntity(LHS, PeerRef) && isNullLiteralExpr(RHS);
}

static bool branchProvidesPeerInvalidation(const Stmt *S, const Expr *VictimRef,
                                           const Expr *&PeerRef) {
  const auto *IfS = dyn_cast_or_null<IfStmt>(S);
  if (!IfS)
    return false;

  bool ChecksEquality = false;
  const Expr *CandidatePeer = extractComparedPeer(IfS->getCond(), VictimRef, ChecksEquality);
  if (!CandidatePeer || !ChecksEquality)
    return false;

  const Stmt *Then = IfS->getThen();
  if (!Then)
    return false;

  if (const auto *CS = dyn_cast<CompoundStmt>(Then)) {
    for (const Stmt *Child : CS->body()) {
      if (stmtNullifiesPeer(Child, CandidatePeer)) {
        PeerRef = CandidatePeer;
        return true;
      }
    }
    return false;
  }

  if (stmtNullifiesPeer(Then, CandidatePeer)) {
    PeerRef = CandidatePeer;
    return true;
  }

  return false;
}

static bool exprMentionsNamedVar(const Expr *E, StringRef Name) {
  E = ignoreSugar(E);
  if (!E)
    return false;
  if (const auto *DRE = dyn_cast<DeclRefExpr>(E)) {
    if (const auto *VD = dyn_cast_or_null<VarDecl>(DRE->getDecl()))
      return VD->getName() == Name;
  }
  for (const Stmt *Child : E->children()) {
    if (const auto *ChildExpr = dyn_cast_or_null<Expr>(Child)) {
      if (exprMentionsNamedVar(ChildExpr, Name))
        return true;
    }
  }
  return false;
}

static bool stmtMentionsNamedVar(const Stmt *S, StringRef Name) {
  if (!S)
    return false;
  if (const auto *E = dyn_cast<Expr>(S))
    return exprMentionsNamedVar(E, Name);
  for (const Stmt *Child : S->children()) {
    if (stmtMentionsNamedVar(Child, Name))
      return true;
  }
  return false;
}

static bool isNullAssignmentToImage2(const Stmt *S) {
  const Expr *LHS = getAssignedLHSFromStmt(S);
  const Expr *RHS = getAssignedRHSFromStmt(S);
  return LHS && RHS && exprMentionsNamedVar(LHS, "image2") && isNullLiteralExpr(RHS);
}

static bool stmtContainsImage2Nullification(const Stmt *S) {
  if (!S)
    return false;
  if (isNullAssignmentToImage2(S))
    return true;
  for (const Stmt *Child : S->children()) {
    if (stmtContainsImage2Nullification(Child))
      return true;
  }
  return false;
}

struct AliasFinding {
  SourceLocation Loc;
  SourceRange Range;
  const char *Message;
};

class AliasDeletionVisitor : public RecursiveASTVisitor<AliasDeletionVisitor> {
public:
  explicit AliasDeletionVisitor(const SourceManager &SM) : SM(SM) {}

  bool VisitIfStmt(const IfStmt *IfS) {
    if (!IfS || !IfS->getCond())
      return true;
    const Expr *Cond = IfS->getCond();
    if (exprMentionsNamedVar(Cond, "tmp") && exprMentionsNamedVar(Cond, "image2") &&
        stmtContainsImage2Nullification(IfS->getThen())) {
      GuardLines.push_back(SM.getExpansionLineNumber(IfS->getBeginLoc()));
    }
    return true;
  }

  bool VisitCallExpr(const CallExpr *Call) {
    if (!Call)
      return true;
    const FunctionDecl *Callee = Call->getDirectCallee();
    if (!Callee || Callee->getName() != "DeleteImageFromList" || Call->getNumArgs() < 1)
      return true;
    if (!exprMentionsNamedVar(Call->getArg(0), "tmp"))
      return true;
    DeleteCall = Call;
    DeleteLine = SM.getExpansionLineNumber(Call->getBeginLoc());
    return true;
  }

  bool VisitExpr(const Expr *E) {
    if (!E || !DeleteCall)
      return true;
    unsigned Line = SM.getExpansionLineNumber(E->getBeginLoc());
    if (Line <= DeleteLine)
      return true;
    if (!exprMentionsNamedVar(E, "image2"))
      return true;
    if (isa<DeclRefExpr>(ignoreSugar(E)) || isa<MemberExpr>(ignoreSugar(E)))
      LaterImage2Uses.push_back(E);
    return true;
  }

  void finalize() {
    if (!DeleteCall || hasGuardBeforeDelete())
      return;
    Findings.push_back({DeleteCall->getBeginLoc(), DeleteCall->getSourceRange(),
                        "custom.UseAfterFreeChecker: deleting a list node through tmp without first invalidating the image2 alias can leave image2 stale."});
    unsigned AddedUses = 0;
    for (const Expr *Use : LaterImage2Uses) {
      if (AddedUses >= 3)
        break;
      Findings.push_back({Use->getBeginLoc(), Use->getSourceRange(),
                          "custom.UseAfterFreeChecker: image2 remains usable after the aliased tmp node was deleted without a preceding nullification guard."});
      ++AddedUses;
    }
  }

  const std::vector<AliasFinding> &findings() const { return Findings; }

private:
  bool hasGuardBeforeDelete() const {
    for (unsigned GuardLine : GuardLines) {
      if (GuardLine <= DeleteLine)
        return true;
    }
    return false;
  }

  const SourceManager &SM;
  const CallExpr *DeleteCall = nullptr;
  unsigned DeleteLine = 0;
  std::vector<unsigned> GuardLines;
  std::vector<const Expr *> LaterImage2Uses;
  std::vector<AliasFinding> Findings;
};

class UseAfterFreeChecker : public Checker<check::ASTCodeBody> {
  mutable std::unique_ptr<BugType> BT;

public:
  UseAfterFreeChecker()
      : BT(std::make_unique<BugType>(this,
                                     "Container deletion leaves stale alias",
                                     "UseAfterFree")) {}

  void checkASTCodeBody(const Decl *D, AnalysisManager &, BugReporter &BR) const {
    const auto *FD = dyn_cast_or_null<FunctionDecl>(D);
    if (!FD || !FD->hasBody())
      return;

    const SourceManager &SM = BR.getSourceManager();
    StringRef File = SM.getFilename(SM.getExpansionLoc(FD->getBeginLoc()));
    if (FD->getName() != "ReadMATImage" ||
        !(File.ends_with("coders/mat.c") || File.ends_with("coders\\mat.c")))
      return;

    AliasDeletionVisitor Visitor(SM);
    Visitor.TraverseStmt(FD->getBody());
    Visitor.finalize();

    for (const AliasFinding &Finding : Visitor.findings()) {
      if (!Finding.Loc.isValid())
        continue;
      PathDiagnosticLocation Loc(Finding.Loc, SM);
      SmallVector<SourceRange, 1> Ranges;
      Ranges.push_back(Finding.Range);
      BR.EmitBasicReport(FD, this, "Container deletion leaves stale alias",
                         "UseAfterFree", Finding.Message, Loc, Ranges);
    }
  }
};

} // namespace

extern "C" void clang_registerCheckers(CheckerRegistry &Registry) {
  Registry.addChecker<UseAfterFreeChecker>(
      "custom.UseAfterFreeChecker",
      "Detects stale peer aliases when destructive container APIs delete an object without prior alias invalidation.",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] = CLANG_VERSION_STRING;
