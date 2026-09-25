#include "clang/AST/Expr.h"
#include "clang/AST/StmtVisitor.h"
#include "clang/Basic/Version.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/CheckerManager.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SVals.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include "llvm/ADT/SmallVector.h"
#include <memory>

using namespace clang;
using namespace ento;

namespace {

class OutOfBoundsLookaheadReadChecker
    : public Checker<check::Location, check::BranchCondition> {
  mutable std::unique_ptr<BugType> BT;

public:
  OutOfBoundsLookaheadReadChecker()
      : BT(std::make_unique<BugType>(this, "Unchecked lookahead read",
                                     "Memory safety")) {}

  void checkLocation(SVal Loc, bool IsLoad, const Stmt *S,
                     CheckerContext &C) const;
  void checkBranchCondition(const Stmt *Condition, CheckerContext &C) const;

private:
  struct IndexInfo {
    const ValueDecl *BaseDecl = nullptr;
    const ValueDecl *IndexDecl = nullptr;
    llvm::APSInt Offset;
    bool Valid = false;

    IndexInfo()
        : Offset(llvm::APInt(32, 0), true) {}
  };

  struct GuardContext {
    const Stmt *LoopOrIf = nullptr;
    const Expr *Cond = nullptr;
    bool Protected = false;
  };

  static const Expr *ignore(const Expr *E) {
    return E ? E->IgnoreParenCasts() : nullptr;
  }

  static bool getIntegerLiteralValue(const Expr *E, llvm::APSInt &Out) {
    E = ignore(E);
    if (const auto *IL = dyn_cast_or_null<IntegerLiteral>(E)) {
      Out = IL->getValue();
      return true;
    }
    return false;
  }

  static const ValueDecl *getDeclFromExpr(const Expr *E) {
    E = ignore(E);
    if (const auto *DRE = dyn_cast_or_null<DeclRefExpr>(E))
      return DRE->getDecl();
    return nullptr;
  }

  static bool collectIndexInfo(const Expr *Index, IndexInfo &Info) {
    Index = ignore(Index);
    if (!Index)
      return false;

    if (const auto *DRE = dyn_cast<DeclRefExpr>(Index)) {
      Info.IndexDecl = DRE->getDecl();
      Info.Offset = llvm::APSInt(llvm::APInt(32, 0), true);
      Info.Valid = true;
      return true;
    }

    if (const auto *BO = dyn_cast<BinaryOperator>(Index)) {
      if (!(BO->getOpcode() == BO_Add || BO->getOpcode() == BO_Sub))
        return false;

      const Expr *LHS = ignore(BO->getLHS());
      const Expr *RHS = ignore(BO->getRHS());
      if (!LHS || !RHS)
        return false;

      llvm::APSInt C(llvm::APInt(32, 0), true);
      if (const auto *DRE = dyn_cast<DeclRefExpr>(LHS)) {
        if (!getIntegerLiteralValue(RHS, C))
          return false;
        Info.IndexDecl = DRE->getDecl();
        Info.Offset = C;
        if (BO->getOpcode() == BO_Sub)
          Info.Offset = -Info.Offset;
        Info.Valid = true;
        return true;
      }

      if (const auto *DRE = dyn_cast<DeclRefExpr>(RHS)) {
        if (BO->getOpcode() != BO_Add)
          return false;
        if (!getIntegerLiteralValue(LHS, C))
          return false;
        Info.IndexDecl = DRE->getDecl();
        Info.Offset = C;
        Info.Valid = true;
        return true;
      }
    }

    return false;
  }

  static bool getBaseDecl(const Expr *Base, const ValueDecl *&Decl) {
    Base = ignore(Base);
    if (!Base)
      return false;

    if (const auto *DRE = dyn_cast<DeclRefExpr>(Base)) {
      Decl = DRE->getDecl();
      return true;
    }

    if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(Base))
      return getBaseDecl(ASE->getBase(), Decl);

    if (const auto *ME = dyn_cast<MemberExpr>(Base)) {
      if (const ValueDecl *VD = dyn_cast_or_null<ValueDecl>(ME->getMemberDecl())) {
        Decl = VD;
        return true;
      }
    }

    if (const auto *UO = dyn_cast<UnaryOperator>(Base)) {
      if (UO->getOpcode() == UO_Deref)
        return getBaseDecl(UO->getSubExpr(), Decl);
    }

    return false;
  }

  static bool extractArrayAccess(const Expr *E, IndexInfo &Info,
                                 const Expr *&IndexExpr) {
    E = ignore(E);
    const auto *ASE = dyn_cast_or_null<ArraySubscriptExpr>(E);
    if (!ASE)
      return false;

    const ValueDecl *BaseDecl = nullptr;
    if (!getBaseDecl(ASE->getBase(), BaseDecl))
      return false;

    if (!collectIndexInfo(ASE->getIdx(), Info))
      return false;

    Info.BaseDecl = BaseDecl;
    IndexExpr = ASE->getIdx();
    return Info.Valid && Info.BaseDecl && Info.IndexDecl;
  }

  static bool exprMentionsDecl(const Expr *E, const ValueDecl *VD) {
    E = ignore(E);
    if (!E || !VD)
      return false;

    if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
      return DRE->getDecl() == VD;

    for (const Stmt *Child : E->children()) {
      const auto *ChildExpr = dyn_cast_or_null<Expr>(Child);
      if (ChildExpr && exprMentionsDecl(ChildExpr, VD))
        return true;
    }
    return false;
  }

  static bool isSameBaseLengthRef(const Expr *E, const ValueDecl *LenDecl) {
    return exprMentionsDecl(E, LenDecl);
  }

  static bool isOffsetProtectedByUpperBound(const Expr *Cond,
                                            const ValueDecl *IndexDecl,
                                            const ValueDecl *LengthDecl,
                                            const llvm::APSInt &Offset);
  static bool hasProtectiveGuard(const Expr *Cond,
                                 const ValueDecl *IndexDecl,
                                 const ValueDecl *LengthDecl,
                                 const llvm::APSInt &Offset);
  static GuardContext findEnclosingGuard(const Stmt *Root, const Stmt *Target,
                                         const ValueDecl *IndexDecl,
                                         const llvm::APSInt &Offset);
  static bool isProtectedByContainingCondition(const Stmt *Root,
                                               const Stmt *Target,
                                               const ValueDecl *IndexDecl,
                                               const ValueDecl *LengthDecl,
                                               const llvm::APSInt &Offset);
};

bool OutOfBoundsLookaheadReadChecker::isOffsetProtectedByUpperBound(
    const Expr *Cond, const ValueDecl *IndexDecl, const ValueDecl *LengthDecl,
    const llvm::APSInt &Offset) {
  Cond = ignore(Cond);
  if (!Cond || !IndexDecl || !LengthDecl)
    return false;

  const auto *BO = dyn_cast<BinaryOperator>(Cond);
  if (!BO)
    return false;

  BinaryOperatorKind Op = BO->getOpcode();
  if (!(Op == BO_LT || Op == BO_LE || Op == BO_GT || Op == BO_GE ||
        Op == BO_EQ || Op == BO_NE || Op == BO_LAnd || Op == BO_LOr))
    return false;

  if (Op == BO_LAnd || Op == BO_LOr) {
    return hasProtectiveGuard(BO->getLHS(), IndexDecl, LengthDecl, Offset) ||
           hasProtectiveGuard(BO->getRHS(), IndexDecl, LengthDecl, Offset);
  }

  const Expr *LHS = ignore(BO->getLHS());
  const Expr *RHS = ignore(BO->getRHS());
  if (!LHS || !RHS)
    return false;

  IndexInfo Info;
  if (collectIndexInfo(LHS, Info) && Info.IndexDecl == IndexDecl &&
      isSameBaseLengthRef(RHS, LengthDecl)) {
    llvm::APSInt Needed = Offset;
    if (Op == BO_LT)
      return Info.Offset >= Needed;
    if (Op == BO_LE)
      return Info.Offset > Needed;
  }

  Info = IndexInfo();
  if (collectIndexInfo(RHS, Info) && Info.IndexDecl == IndexDecl &&
      isSameBaseLengthRef(LHS, LengthDecl)) {
    llvm::APSInt Needed = Offset;
    if (Op == BO_GT)
      return Info.Offset >= Needed;
    if (Op == BO_GE)
      return Info.Offset > Needed;
  }

  return false;
}

bool OutOfBoundsLookaheadReadChecker::hasProtectiveGuard(
    const Expr *Cond, const ValueDecl *IndexDecl, const ValueDecl *LengthDecl,
    const llvm::APSInt &Offset) {
  Cond = ignore(Cond);
  if (!Cond)
    return false;

  if (const auto *UO = dyn_cast<UnaryOperator>(Cond)) {
    if (UO->getOpcode() == UO_LNot)
      return hasProtectiveGuard(UO->getSubExpr(), IndexDecl, LengthDecl, Offset);
  }

  if (isOffsetProtectedByUpperBound(Cond, IndexDecl, LengthDecl, Offset))
    return true;

  if (const auto *BO = dyn_cast<BinaryOperator>(Cond)) {
    if (BO->isLogicalOp())
      return hasProtectiveGuard(BO->getLHS(), IndexDecl, LengthDecl, Offset) ||
             hasProtectiveGuard(BO->getRHS(), IndexDecl, LengthDecl, Offset);
  }

  return false;
}

OutOfBoundsLookaheadReadChecker::GuardContext
OutOfBoundsLookaheadReadChecker::findEnclosingGuard(
    const Stmt *Root, const Stmt *Target, const ValueDecl *IndexDecl,
    const llvm::APSInt &Offset) {
  GuardContext Result;
  if (!Root || !Target || !IndexDecl)
    return Result;

  class GuardFinder : public ConstStmtVisitor<GuardFinder> {
    const Stmt *Target;
    const ValueDecl *IndexDecl;
    const llvm::APSInt &Offset;
    GuardContext Result;

  public:
    GuardFinder(const Stmt *TargetStmt, const ValueDecl *Idx,
                const llvm::APSInt &Off)
        : Target(TargetStmt), IndexDecl(Idx), Offset(Off) {}

    GuardContext run(const Stmt *Node) {
      Visit(Node);
      return Result;
    }

    void VisitStmt(const Stmt *Node) {
      if (!Node || Result.Protected)
        return;
      for (const Stmt *Child : Node->children())
        if (Child)
          Visit(Child);
    }

    void VisitIfStmt(const IfStmt *IfS) {
      if (Result.Protected)
        return;
      const Stmt *Then = IfS->getThen();
      const Stmt *Else = IfS->getElse();
      if (Then && containsStmt(Then, Target) && checkCond(IfS, IfS->getCond()))
        return;
      if (Else && containsStmt(Else, Target))
        return;
      VisitStmt(IfS);
    }

    void VisitWhileStmt(const WhileStmt *WS) {
      if (Result.Protected)
        return;
      const Stmt *Body = WS->getBody();
      if (Body && containsStmt(Body, Target) && checkCond(WS, WS->getCond()))
        return;
      VisitStmt(WS);
    }

    void VisitForStmt(const ForStmt *FS) {
      if (Result.Protected)
        return;
      const Stmt *Body = FS->getBody();
      if (Body && containsStmt(Body, Target) && checkCond(FS, FS->getCond()))
        return;
      VisitStmt(FS);
    }

  private:
    static bool containsStmt(const Stmt *Root, const Stmt *Needle) {
      if (!Root)
        return false;
      if (Root == Needle)
        return true;
      for (const Stmt *Child : Root->children())
        if (Child && containsStmt(Child, Needle))
          return true;
      return false;
    }

    bool checkCond(const Stmt *Owner, const Expr *Cond) {
      const ValueDecl *LengthDecl = findLengthDecl(Cond);
      if (!LengthDecl)
        return false;
      if (!OutOfBoundsLookaheadReadChecker::hasProtectiveGuard(
              Cond, IndexDecl, LengthDecl, Offset))
        return false;
      Result.LoopOrIf = Owner;
      Result.Cond = Cond;
      Result.Protected = true;
      return true;
    }

    const ValueDecl *findLengthDecl(const Expr *E) {
      E = OutOfBoundsLookaheadReadChecker::ignore(E);
      if (!E)
        return nullptr;

      if (const auto *BO = dyn_cast<BinaryOperator>(E)) {
        const Expr *LHS = OutOfBoundsLookaheadReadChecker::ignore(BO->getLHS());
        const Expr *RHS = OutOfBoundsLookaheadReadChecker::ignore(BO->getRHS());

        OutOfBoundsLookaheadReadChecker::IndexInfo Info;
        if (OutOfBoundsLookaheadReadChecker::collectIndexInfo(LHS, Info) &&
            Info.IndexDecl == IndexDecl) {
          if (const ValueDecl *VD =
                  OutOfBoundsLookaheadReadChecker::getDeclFromExpr(RHS))
            return VD;
        }

        Info = OutOfBoundsLookaheadReadChecker::IndexInfo();
        if (OutOfBoundsLookaheadReadChecker::collectIndexInfo(RHS, Info) &&
            Info.IndexDecl == IndexDecl) {
          if (const ValueDecl *VD =
                  OutOfBoundsLookaheadReadChecker::getDeclFromExpr(LHS))
            return VD;
        }
      }

      for (const Stmt *Child : E->children()) {
        const auto *ChildExpr = dyn_cast_or_null<Expr>(Child);
        if (!ChildExpr)
          continue;
        if (const ValueDecl *VD = findLengthDecl(ChildExpr))
          return VD;
      }
      return nullptr;
    }
  };

  return GuardFinder(Target, IndexDecl, Offset).run(Root);
}

bool OutOfBoundsLookaheadReadChecker::isProtectedByContainingCondition(
    const Stmt *Root, const Stmt *Target, const ValueDecl *IndexDecl,
    const ValueDecl *LengthDecl, const llvm::APSInt &Offset) {
  if (!Root || !Target || !IndexDecl || !LengthDecl)
    return false;

  llvm::SmallVector<const Stmt *, 16> Path;

  class PathFinder : public ConstStmtVisitor<PathFinder> {
    const Stmt *Target;
    llvm::SmallVectorImpl<const Stmt *> &Path;
    bool Found = false;

  public:
    PathFinder(const Stmt *TargetStmt, llvm::SmallVectorImpl<const Stmt *> &OutPath)
        : Target(TargetStmt), Path(OutPath) {}

    bool run(const Stmt *Node) {
      Visit(Node);
      return Found;
    }

    void VisitStmt(const Stmt *Node) {
      if (!Node || Found)
        return;
      Path.push_back(Node);
      if (Node == Target) {
        Found = true;
        return;
      }
      for (const Stmt *Child : Node->children()) {
        if (!Child || Found)
          continue;
        Visit(Child);
      }
      if (!Found)
        Path.pop_back();
    }
  };

  if (!PathFinder(Target, Path).run(Root))
    return false;

  for (size_t I = 0; I + 1 < Path.size(); ++I) {
    const Stmt *Node = Path[I];
    const Stmt *Next = Path[I + 1];

    if (const auto *IfS = dyn_cast<IfStmt>(Node)) {
      if (IfS->getThen() && IfS->getThen() == Next &&
          hasProtectiveGuard(IfS->getCond(), IndexDecl, LengthDecl, Offset))
        return true;
      continue;
    }

    if (const auto *BO = dyn_cast<BinaryOperator>(Node)) {
      if (!BO->isLogicalOp())
        continue;
      if (BO->getOpcode() == BO_LAnd && BO->getRHS() == Next &&
          hasProtectiveGuard(BO->getLHS(), IndexDecl, LengthDecl, Offset))
        return true;
      if (BO->getOpcode() == BO_LOr && BO->getRHS() == Next) {
        const Expr *LHS = ignore(BO->getLHS());
        if (const auto *UO = dyn_cast_or_null<UnaryOperator>(LHS)) {
          if (UO->getOpcode() == UO_LNot &&
              hasProtectiveGuard(UO->getSubExpr(), IndexDecl, LengthDecl,
                                 Offset))
            return true;
        }
      }
      continue;
    }

    if (const auto *CO = dyn_cast<ConditionalOperator>(Node)) {
      if (CO->getTrueExpr() == Next &&
          hasProtectiveGuard(CO->getCond(), IndexDecl, LengthDecl, Offset))
        return true;
      const Expr *Cond = ignore(CO->getCond());
      if (CO->getFalseExpr() == Next) {
        if (const auto *UO = dyn_cast_or_null<UnaryOperator>(Cond)) {
          if (UO->getOpcode() == UO_LNot &&
              hasProtectiveGuard(UO->getSubExpr(), IndexDecl, LengthDecl,
                                 Offset))
            return true;
        }
      }
    }
  }

  return false;
}

void OutOfBoundsLookaheadReadChecker::checkBranchCondition(
    const Stmt *Condition, CheckerContext &C) const {
  (void)Condition;
  (void)C;
}

void OutOfBoundsLookaheadReadChecker::checkLocation(SVal Loc, bool IsLoad,
                                                    const Stmt *S,
                                                    CheckerContext &C) const {
  if (!IsLoad || !S)
    return;

  const auto *E = dyn_cast<Expr>(S);
  if (!E)
    return;

  IndexInfo Access;
  const Expr *IndexExpr = nullptr;
  if (!extractArrayAccess(E, Access, IndexExpr))
    return;

  if (!Access.Valid || !Access.BaseDecl || !Access.IndexDecl)
    return;

  if (Access.Offset <= 0)
    return;

  const Stmt *Body = C.getCurrentAnalysisDeclContext()->getBody();
  GuardContext Guard =
      findEnclosingGuard(Body, S, Access.IndexDecl, Access.Offset);
  if (Guard.Protected)
    return;

  if (isProtectedByContainingCondition(Body, S, Access.IndexDecl,
                                       Access.BaseDecl, Access.Offset))
    return;

  (void)Loc;

  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N)
    return;

  auto R = std::make_unique<PathSensitiveBugReport>(
      *BT,
      "Array lookahead read uses index + constant offset without an explicit "
      "upper-bound guard proving the extra byte is within the buffer length.",
      N);
  R->addRange(E->getSourceRange());
  if (IndexExpr)
    R->addRange(IndexExpr->getSourceRange());
  C.emitReport(std::move(R));
}

} // namespace

extern "C" void clang_registerCheckers(CheckerRegistry &Registry) {
  Registry.addChecker<OutOfBoundsLookaheadReadChecker>(
      "custom.OutOfBoundsLookaheadReadChecker",
      "Detect unchecked lookahead array reads lacking a matching upper-bound guard.",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] = CLANG_VERSION_STRING;
