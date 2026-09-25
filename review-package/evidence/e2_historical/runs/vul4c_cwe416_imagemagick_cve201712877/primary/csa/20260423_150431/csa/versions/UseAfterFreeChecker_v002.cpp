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

    const auto *Body = dyn_cast<CompoundStmt>(FD->getBody());
    if (!Body)
      return;

    const Stmt *Prev = nullptr;
    const Expr *PendingVictim = nullptr;
    const Expr *GuardedPeer = nullptr;

    for (const Stmt *S : Body->body()) {
      const auto *Call = dyn_cast<CallExpr>(S);
      if (!Call) {
        Prev = S;
        continue;
      }

      const FunctionDecl *Callee = Call->getDirectCallee();
      if (!Callee) {
        Prev = S;
        continue;
      }

      StringRef Name = Callee->getName();
      if (!Name.contains("Delete") && !Name.contains("Destroy") &&
          !Name.contains("Remove")) {
        Prev = S;
        continue;
      }

      if (Call->getNumArgs() < 1) {
        Prev = S;
        continue;
      }

      const Expr *Arg0 = ignoreSugar(Call->getArg(0));
      if (!Arg0) {
        Prev = S;
        continue;
      }

      const Expr *VictimRef = nullptr;
      if (const auto *UO = dyn_cast<UnaryOperator>(Arg0)) {
        if (UO->getOpcode() == UO_AddrOf)
          VictimRef = ignoreSugar(UO->getSubExpr());
      }
      if (!VictimRef) {
        Prev = S;
        continue;
      }

      PendingVictim = VictimRef;
      GuardedPeer = nullptr;

      if (Prev)
        branchProvidesPeerInvalidation(Prev, PendingVictim, GuardedPeer);

      if (!GuardedPeer) {
        const SourceManager &SM = BR.getSourceManager();
        PathDiagnosticLocation Loc(Call->getBeginLoc(), SM);
        SmallVector<SourceRange, 2> Ranges;
        Ranges.push_back(Call->getSourceRange());
        if (PendingVictim)
          Ranges.push_back(PendingVictim->getSourceRange());
        BR.EmitBasicReport(FD, this, "Container deletion leaves stale alias", "UseAfterFree",
                           "Deleting or removing a container-owned object through one alias without first invalidating equivalent peer aliases can leave stale pointers that may be used after free.",
                           Loc, Ranges);
        return;
      }

      Prev = S;
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
