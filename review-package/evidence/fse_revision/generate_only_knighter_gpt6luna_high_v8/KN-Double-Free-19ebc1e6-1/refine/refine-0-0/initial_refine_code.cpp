#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/Analysis/PathDiagnostic.h"
#include "clang/StaticAnalyzer/Checkers/Taint.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/Environment.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramState.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramStateTrait.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SymExpr.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtVisitor.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/StaticAnalyzer/Checkers/utility.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/StmtVisitor.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"
#include <map>
#include <set>
#include <vector>
#include <string>
#include <algorithm>

using namespace clang;
using namespace ento;
using namespace taint;

namespace {
class SAGenTestChecker : public Checker<check::ASTCodeBody> {
  mutable std::unique_ptr<BugType> BT;

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Double free on retry path", "Memory Management")) {}

  void checkASTCodeBody(const Decl *D, AnalysisManager &Mgr,
                        BugReporter &BR) const;

private:
  struct LabelInfo {
    const LabelDecl *LD = nullptr;
    std::string Name;
    SourceLocation Loc;
    const LabelStmt *LS = nullptr;
  };

  struct GotoInfo {
    const GotoStmt *GS = nullptr;
    const LabelDecl *Target = nullptr;
    SourceLocation FromLoc;
  };

  struct FreeInfo {
    const CallExpr *CE = nullptr;
    const VarDecl *VD = nullptr;
    SourceLocation Loc;
    const LabelDecl *CleanupLabel = nullptr;
  };

  struct ResetInfo {
    const VarDecl *VD = nullptr;
    SourceLocation Loc;
    const Stmt *S = nullptr;
  };

  struct AllocInfo {
    const VarDecl *VD = nullptr;
    SourceLocation Loc;
    const CallExpr *CE = nullptr;
  };

  struct WriteInfo {
    const VarDecl *VD = nullptr;
    SourceLocation Loc;
    const Stmt *S = nullptr;
  };

  class BodyScanner : public RecursiveASTVisitor<BodyScanner> {
  public:
    BodyScanner(ASTContext &AC, const SourceManager &SM)
        : Ctx(AC), SM(SM) {}

    std::vector<LabelInfo> Labels;
    std::map<const LabelDecl *, LabelInfo> LabelMap;
    std::vector<GotoInfo> Gotos;
    std::vector<FreeInfo> Frees;
    std::map<const LabelDecl *, std::vector<const VarDecl *>> CleanupFrees;
    std::map<const VarDecl *, std::vector<ResetInfo>> Resets;
    std::map<const VarDecl *, std::vector<AllocInfo>> Allocs;
    std::map<const VarDecl *, std::vector<WriteInfo>> Writes;

    static bool isFreeLike(const FunctionDecl *FD) {
      if (!FD)
        return false;
      if (const IdentifierInfo *II = FD->getIdentifier()) {
        StringRef N = II->getName();
        return N == "kfree" || N == "kvfree" || N == "vfree";
      }
      return false;
    }

    static bool isAllocLike(const FunctionDecl *FD) {
      if (!FD)
        return false;
      if (const IdentifierInfo *II = FD->getIdentifier()) {
        StringRef N = II->getName();
        return N == "kmalloc" || N == "kzalloc" || N == "kcalloc" ||
               N == "kmalloc_array" || N == "kvmalloc" || N == "kvzalloc" ||
               N == "vmalloc";
      }
      return false;
    }

    bool isNullExpr(const Expr *E) const {
      if (!E)
        return false;
      E = E->IgnoreParenImpCasts();
      if (E->isNullPointerConstant(Ctx, Expr::NPC_ValueDependentIsNull))
        return true;
      if (isa<GNUNullExpr>(E))
        return true;
      if (const IntegerLiteral *IL = dyn_cast<IntegerLiteral>(E))
        return IL->getValue() == 0;

      CharSourceRange Range = CharSourceRange::getTokenRange(E->getSourceRange());
      StringRef Text = Lexer::getSourceText(Range, SM, Ctx.getLangOpts());
      return Text.contains("NULL");
    }

    // Writes nested in branches, loops, or short-circuit expressions do not
    // establish that the pointer is refreshed on every path to a later goto.
    bool isUnconditionalWrite(const Stmt *S) const {
      const Stmt *Current = S;
      while (Current) {
        auto Parents = Ctx.getParents(*Current);
        if (Parents.empty())
          break;

        const Stmt *Parent = Parents[0].get<Stmt>();
        if (!Parent)
          break;

        if (isa<IfStmt>(Parent) || isa<ForStmt>(Parent) ||
            isa<WhileStmt>(Parent) || isa<DoStmt>(Parent) ||
            isa<SwitchStmt>(Parent) || isa<ConditionalOperator>(Parent) ||
            isa<BinaryConditionalOperator>(Parent))
          return false;

        if (const auto *BO = dyn_cast<BinaryOperator>(Parent)) {
          if (BO->getOpcode() == BO_LAnd || BO->getOpcode() == BO_LOr)
            return false;
        }

        if (isa<FunctionDecl>(Parents[0].get<Decl>()))
          break;

        Current = Parent;
      }
      return true;
    }

    bool VisitStmt(Stmt *S) {
      if (!S)
        return true;

      if (auto *LS = dyn_cast<LabelStmt>(S)) {
        LabelInfo LI;
        LI.LD = LS->getDecl();
        if (LI.LD)
          LI.Name = LI.LD->getNameAsString();
        LI.Loc = LS->getBeginLoc();
        LI.LS = LS;
        Labels.push_back(LI);
        if (LI.LD)
          LabelMap[LI.LD] = LI;
      } else if (auto *GS = dyn_cast<GotoStmt>(S)) {
        GotoInfo GI;
        GI.GS = GS;
        GI.Target = GS->getLabel();
        GI.FromLoc = GS->getGotoLoc();
        Gotos.push_back(GI);
      } else if (auto *BO = dyn_cast<BinaryOperator>(S)) {
        if (BO->getOpcode() == BO_Assign) {
          const Expr *LHS = BO->getLHS()->IgnoreParenImpCasts();
          const Expr *RHS = BO->getRHS()->IgnoreParenImpCasts();
          const DeclRefExpr *LDRE = dyn_cast<DeclRefExpr>(LHS);
          const VarDecl *VD =
              LDRE ? dyn_cast<VarDecl>(LDRE->getDecl()) : nullptr;

          if (VD && VD->hasLocalStorage() && VD->getType()->isPointerType()) {
            if (isNullExpr(RHS)) {
              ResetInfo RI;
              RI.VD = VD;
              RI.Loc = BO->getExprLoc();
              RI.S = BO;
              Resets[VD].push_back(RI);
            } else {
              WriteInfo WI;
              WI.VD = VD;
              WI.Loc = BO->getExprLoc();
              WI.S = BO;
              Writes[VD].push_back(WI);

              if (const CallExpr *RCE = dyn_cast<CallExpr>(RHS)) {
                const FunctionDecl *FD = RCE->getDirectCallee();
                if (isAllocLike(FD)) {
                  AllocInfo AI;
                  AI.VD = VD;
                  AI.Loc = BO->getExprLoc();
                  AI.CE = RCE;
                  Allocs[VD].push_back(AI);
                }
              }
            }
          }
        }
      } else if (auto *DS = dyn_cast<DeclStmt>(S)) {
        for (auto *D : DS->decls()) {
          if (auto *VD = dyn_cast<VarDecl>(D)) {
            if (!VD->hasLocalStorage() || !VD->getType()->isPointerType())
              continue;

            if (const Expr *Init = VD->getInit()) {
              Init = Init->IgnoreParenImpCasts();
              if (isNullExpr(Init)) {
                ResetInfo RI;
                RI.VD = VD;
                RI.Loc = DS->getBeginLoc();
                RI.S = DS;
                Resets[VD].push_back(RI);
              } else {
                WriteInfo WI;
                WI.VD = VD;
                WI.Loc = DS->getBeginLoc();
                WI.S = DS;
                Writes[VD].push_back(WI);

                if (const CallExpr *CE = dyn_cast<CallExpr>(Init)) {
                  const FunctionDecl *FD = CE->getDirectCallee();
                  if (isAllocLike(FD)) {
                    AllocInfo AI;
                    AI.VD = VD;
                    AI.Loc = DS->getBeginLoc();
                    AI.CE = CE;
                    Allocs[VD].push_back(AI);
                  }
                }
              }
            }
          }
        }
      } else if (auto *CE = dyn_cast<CallExpr>(S)) {
        const FunctionDecl *FD = CE->getDirectCallee();
        if (isFreeLike(FD) && CE->getNumArgs() >= 1) {
          const Expr *Arg0 = CE->getArg(0);
          Arg0 = Arg0 ? Arg0->IgnoreParenImpCasts() : nullptr;
          const DeclRefExpr *DRE = Arg0 ? dyn_cast<DeclRefExpr>(Arg0) : nullptr;
          const VarDecl *VD =
              DRE ? dyn_cast<VarDecl>(DRE->getDecl()) : nullptr;

          if (VD && VD->hasLocalStorage() && VD->getType()->isPointerType()) {
            FreeInfo FI;
            FI.CE = CE;
            FI.VD = VD;
            FI.Loc = CE->getExprLoc();
            FI.CleanupLabel = findDominatingLabel(FI.Loc);
            Frees.push_back(FI);
            if (FI.CleanupLabel)
              CleanupFrees[FI.CleanupLabel].push_back(VD);
          }
        }
      }

      return true;
    }

    const LabelDecl *findDominatingLabel(SourceLocation Loc) const {
      const LabelDecl *Best = nullptr;
      SourceLocation BestLoc;
      for (const auto &LI : Labels) {
        SourceLocation L = LI.Loc;
        if (SM.isBeforeInTranslationUnit(L, Loc) &&
            (!Best || SM.isBeforeInTranslationUnit(BestLoc, L))) {
          Best = LI.LD;
          BestLoc = L;
        }
      }
      return Best;
    }

  private:
    ASTContext &Ctx;
    const SourceManager &SM;
  };

  static bool isBefore(const SourceManager &SM, SourceLocation A,
                       SourceLocation B) {
    return SM.isBeforeInTranslationUnit(A, B);
  }

  static bool isAfter(const SourceManager &SM, SourceLocation A,
                      SourceLocation B) {
    return SM.isBeforeInTranslationUnit(B, A);
  }

  static bool isAfterOrEqual(const SourceManager &SM, SourceLocation A,
                             SourceLocation B) {
    return !SM.isBeforeInTranslationUnit(A, B);
  }

  static SourceLocation getFunctionEndLoc(const Decl *D) {
    if (const auto *FD = dyn_cast<FunctionDecl>(D)) {
      if (const Stmt *Body = FD->getBody())
        return Body->getEndLoc();
    }
    return D->getEndLoc();
  }

  static bool hasUnconditionalWriteBeforeGoto(
      const BodyScanner &Scanner, const SourceManager &SM, const VarDecl *V,
      SourceLocation ReplayLoc, SourceLocation GotoLoc) {
    auto It = Scanner.Writes.find(V);
    if (It == Scanner.Writes.end())
      return false;

    for (const WriteInfo &WI : It->second) {
      if (!WI.S || !Scanner.isUnconditionalWrite(WI.S))
        continue;
      if (isAfter(SM, WI.Loc, ReplayLoc) && isBefore(SM, WI.Loc, GotoLoc))
        return true;
    }
    return false;
  }
};

void SAGenTestChecker::checkASTCodeBody(const Decl *D, AnalysisManager &Mgr,
                                        BugReporter &BR) const {
  const auto *FD = dyn_cast<FunctionDecl>(D);
  if (!FD || !FD->hasBody())
    return;

  ASTContext &Ctx = Mgr.getASTContext();
  const SourceManager &SM = Mgr.getSourceManager();
  const Stmt *Body = FD->getBody();
  if (!Body)
    return;

  BodyScanner Scanner(Ctx, SM);
  Scanner.TraverseStmt(const_cast<Stmt *>(Body));

  if (Scanner.Labels.empty() || Scanner.Gotos.empty() || Scanner.Frees.empty())
    return;

  std::set<const LabelDecl *> ReplayLabels;
  for (const auto &GI : Scanner.Gotos) {
    if (!GI.Target)
      continue;
    auto It = Scanner.LabelMap.find(GI.Target);
    if (It != Scanner.LabelMap.end() &&
        isBefore(SM, It->second.Loc, GI.FromLoc))
      ReplayLabels.insert(GI.Target);
  }

  if (ReplayLabels.empty())
    return;

  std::map<const LabelDecl *, std::vector<FreeInfo>> FreeInfosPerCleanup;
  for (const auto &FI : Scanner.Frees) {
    if (FI.CleanupLabel)
      FreeInfosPerCleanup[FI.CleanupLabel].push_back(FI);
  }

  SourceLocation FuncEndLoc = getFunctionEndLoc(D);

  for (const LabelDecl *La : ReplayLabels) {
    auto LaIt = Scanner.LabelMap.find(La);
    if (LaIt == Scanner.LabelMap.end())
      continue;
    SourceLocation LocA = LaIt->second.Loc;

    for (const auto &LbEntry : FreeInfosPerCleanup) {
      const LabelDecl *Lb = LbEntry.first;
      const auto &FIVec = LbEntry.second;

      for (const FreeInfo &FI : FIVec) {
        const VarDecl *V = FI.VD;
        if (!V)
          continue;

        const GotoStmt *AnchorGoto = nullptr;
        for (const auto &GI : Scanner.Gotos) {
          if (GI.Target == La && isAfter(SM, GI.FromLoc, FI.Loc)) {
            AnchorGoto = GI.GS;
            break;
          }
        }
        if (!AnchorGoto)
          continue;

        SourceLocation LocAlloc = FuncEndLoc;
        bool FoundAlloc = false;
        auto AIIt = Scanner.Allocs.find(V);
        if (AIIt != Scanner.Allocs.end()) {
          for (const auto &AI : AIIt->second) {
            if (isAfter(SM, AI.Loc, LocA) &&
                (!FoundAlloc || isBefore(SM, AI.Loc, LocAlloc))) {
              LocAlloc = AI.Loc;
              FoundAlloc = true;
            }
          }
        }

        bool HasReset = false;
        auto RIIt = Scanner.Resets.find(V);
        if (RIIt != Scanner.Resets.end()) {
          for (const auto &RI : RIIt->second) {
            if (isAfterOrEqual(SM, RI.Loc, LocA) &&
                isBefore(SM, RI.Loc, LocAlloc)) {
              HasReset = true;
              break;
            }
          }
        }
        if (HasReset)
          continue;

        const GotoStmt *EarlyGoto = nullptr;
        for (const auto &GI : Scanner.Gotos) {
          if (GI.Target == Lb &&
              isAfterOrEqual(SM, GI.FromLoc, LocA) &&
              isBefore(SM, GI.FromLoc, LocAlloc)) {
            // A refresh before this goto makes this particular cleanup path
            // safe, even when the assigned value comes from a non-allocator
            // helper such as cifs_convert_path_to_utf16().
            if (hasUnconditionalWriteBeforeGoto(Scanner, SM, V, LocA,
                                                GI.FromLoc))
              continue;
            EarlyGoto = GI.GS;
            break;
          }
        }
        if (!EarlyGoto)
          continue;

        if (!BT)
          return;

        std::string Msg =
            "Pointer freed in cleanup but not reset before retry; "
            "possible double free on replay path";
        PathDiagnosticLocation AnchorLoc =
            PathDiagnosticLocation::createBegin(AnchorGoto, SM, nullptr);
        auto R = std::make_unique<BasicBugReport>(*BT, Msg, AnchorLoc);
        R->addRange(AnchorGoto->getSourceRange());

        if (const IdentifierInfo *II = V->getIdentifier()) {
          R->addNote("Freed pointer: " + II->getName().str(),
                     PathDiagnosticLocation::createBegin(FI.CE, SM, nullptr));
        } else {
          R->addNote("Freed pointer",
                     PathDiagnosticLocation::createBegin(FI.CE, SM, nullptr));
        }
        BR.emitReport(std::move(R));
      }
    }
  }
}

} // namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects double free risk in replay loops when a freed pointer is not reset to NULL before retry",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
