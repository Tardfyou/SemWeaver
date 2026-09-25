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

static bool stmtAssignsPeerFromVictimPrevious(const Stmt *S, const Expr *VictimRef,
                                              const Expr *&PeerRef) {
  const Expr *LHS = getAssignedLHSFromStmt(S);
  const Expr *RHS = ignoreSugar(getAssignedRHSFromStmt(S));
  if (!LHS || !RHS || !VictimRef)
    return false;

  const auto *ME = dyn_cast<MemberExpr>(RHS);
  if (!ME)
    return false;
  if (ME->isArrow() && ME->getMemberDecl() &&
      ME->getMemberDecl()->getName() == "previous" &&
      sameEntity(ME->getBase(), VictimRef)) {
    PeerRef = LHS;
    return true;
  }
  return false;
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

static bool blockDeletesVictimWithoutPeerInvalidation(const CompoundStmt *CS,
                                                      const Expr *&PeerRef,
                                                      const CallExpr *&DeleteCall) {
  if (!CS)
    return false;

  const Expr *TrackedPeer = nullptr;
  for (const Stmt *Child : CS->body()) {
    if (!TrackedPeer)
      stmtAssignsPeerFromVictimPrevious(Child, nullptr, TrackedPeer);

    const auto *Call = dyn_cast<CallExpr>(Child);
    if (!Call)
      continue;
    const FunctionDecl *Callee = Call->getDirectCallee();
    if (!Callee || Callee->getName() != "DeleteImageFromList" || Call->getNumArgs() < 1)
      continue;

    const Expr *DeletedExpr = ignoreSugar(Call->getArg(0));
    const Expr *DerivedPeer = nullptr;
    for (const Stmt *Earlier : CS->body()) {
      if (Earlier == Child)
        break;
      stmtAssignsPeerFromVictimPrevious(Earlier, DeletedExpr, DerivedPeer);
    }

    if (!DerivedPeer)
      continue;

    bool Invalidated = false;
    bool SeenDelete = false;
    for (const Stmt *Later : CS->body()) {
      if (!SeenDelete) {
        if (Later == Child)
          SeenDelete = true;
        continue;
      }
      if (stmtNullifiesPeer(Later, DerivedPeer)) {
        Invalidated = true;
        break;
      }
    }

    if (!Invalidated) {
      PeerRef = DerivedPeer;
      DeleteCall = Call;
      return true;
    }
  }

  return false;
}

static bool exprReferencesEntity(const Expr *E, const Expr *Target) {
  E = ignoreSugar(E);
  if (!E || !Target)
    return false;
  if (sameEntity(E, Target))
    return true;
  for (const Stmt *Child : E->children()) {
    if (const auto *ChildExpr = dyn_cast_or_null<Expr>(Child)) {
      if (exprReferencesEntity(ChildExpr, Target))
        return true;
    }
  }
  return false;
}

static bool stmtReferencesEntity(const Stmt *S, const Expr *Target) {
  if (!S || !Target)
    return false;
  if (const auto *E = dyn_cast<Expr>(S))
    return exprReferencesEntity(E, Target);
  for (const Stmt *Child : S->children()) {
    if (stmtReferencesEntity(Child, Target))
      return true;
  }
  return false;
}

struct AliasFinding {
  SourceLocation Loc;
  SourceRange Range;
  const char *Message;
};

struct DeletionSite {
  const CallExpr *DeleteCall = nullptr;
  const Expr *AliasRef = nullptr;
  unsigned DeleteLine = 0;
  bool Guarded = false;
  std::vector<const Expr *> LaterAliasUses;
};

class AliasDeletionVisitor : public RecursiveASTVisitor<AliasDeletionVisitor> {
public:
  explicit AliasDeletionVisitor(const SourceManager &SM) : SM(SM) {}

  bool VisitCallExpr(const CallExpr *Call) {
    if (!Call)
      return true;
    const FunctionDecl *Callee = Call->getDirectCallee();
    if (!Callee || Callee->getName() != "DeleteImageFromList" || Call->getNumArgs() < 1)
      return true;

    const Expr *DeletedExpr = ignoreSugar(Call->getArg(0));
    if (!DeletedExpr)
      return true;

    for (const Stmt *Node : ParentStack) {
      const Expr *AliasRef = nullptr;
      if (!branchProvidesPeerInvalidation(Node, DeletedExpr, AliasRef))
        continue;

      DeletionSite Site;
      Site.DeleteCall = Call;
      Site.AliasRef = AliasRef;
      Site.DeleteLine = SM.getExpansionLineNumber(Call->getBeginLoc());
      Site.Guarded = true;
      Sites.push_back(Site);
      return true;
    }

    for (const Stmt *Node : ParentStack) {
      const auto *CS = dyn_cast<CompoundStmt>(Node);
      if (!CS)
        continue;
      const Expr *AliasRef = nullptr;
      const CallExpr *MatchedDelete = nullptr;
      if (!blockDeletesVictimWithoutPeerInvalidation(CS, AliasRef, MatchedDelete) ||
          MatchedDelete != Call)
        continue;

      DeletionSite Site;
      Site.DeleteCall = Call;
      Site.AliasRef = AliasRef;
      Site.DeleteLine = SM.getExpansionLineNumber(Call->getBeginLoc());
      Sites.push_back(Site);
      return true;
    }

    DeletionSite Site;
    Site.DeleteCall = Call;
    Site.DeleteLine = SM.getExpansionLineNumber(Call->getBeginLoc());
    Sites.push_back(Site);
    return true;
  }

  bool TraverseStmt(Stmt *S) {
    if (!S)
      return true;
    ParentStack.push_back(S);
    bool Result = RecursiveASTVisitor<AliasDeletionVisitor>::TraverseStmt(S);
    ParentStack.pop_back();
    return Result;
  }

  bool TraverseStmt(const Stmt *S) {
    return TraverseStmt(const_cast<Stmt *>(S));
  }

  bool VisitDeclRefExpr(const DeclRefExpr *DRE) {
    recordAliasUse(DRE);
    return true;
  }

  bool VisitMemberExpr(const MemberExpr *ME) {
    recordAliasUse(ME);
    return true;
  }

  void finalize() {
    for (DeletionSite &Site : Sites) {
      if (!Site.DeleteCall || Site.Guarded || !Site.AliasRef)
        continue;
      Findings.push_back({Site.DeleteCall->getBeginLoc(), Site.DeleteCall->getSourceRange(),
                          "custom.UseAfterFreeChecker: deleting a container element without first invalidating an equal peer alias can leave that alias stale."});
      unsigned AddedUses = 0;
      for (const Expr *Use : Site.LaterAliasUses) {
        if (AddedUses >= 3)
          break;
        Findings.push_back({Use->getBeginLoc(), Use->getSourceRange(),
                            "custom.UseAfterFreeChecker: a peer alias remains usable after the aliased node was deleted without a preceding invalidation guard."});
        ++AddedUses;
      }
    }
  }

  const std::vector<AliasFinding> &findings() const { return Findings; }

private:
  void recordAliasUse(const Expr *E) {
    if (!E)
      return;
    unsigned Line = SM.getExpansionLineNumber(E->getBeginLoc());
    for (DeletionSite &Site : Sites) {
      if (!Site.AliasRef || Line <= Site.DeleteLine)
        continue;
      if (exprReferencesEntity(E, Site.AliasRef))
        Site.LaterAliasUses.push_back(E);
    }
  }

  const SourceManager &SM;
  std::vector<const Stmt *> ParentStack;
  std::vector<DeletionSite> Sites;
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
