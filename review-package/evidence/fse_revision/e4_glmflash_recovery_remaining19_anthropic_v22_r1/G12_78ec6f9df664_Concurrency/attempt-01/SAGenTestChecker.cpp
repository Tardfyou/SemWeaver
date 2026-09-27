#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
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
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/Type.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/DenseMap.h"
#include <map>
#include <string>
#include <utility>
#include <memory>

using namespace clang;
using namespace ento;
using namespace taint;

// No custom program state is necessary for this checker.

namespace {

enum class PerCpuKind { Unknown, ThisCPU, RemoteCPU };

class SAGenTestChecker
    : public Checker<check::ASTCodeBody, check::EndAnalysis> {

  mutable std::unique_ptr<BugType> BT;

  // Key: "<RecordName>.<FieldName>"
  using SiteList = llvm::SmallVector<const Stmt *, 4>;
  std::map<std::string, SiteList> LocalRMWSites;     // from this_cpu_ptr
  std::map<std::string, SiteList> RemoteWriteSites;  // from per_cpu_ptr

public:
  SAGenTestChecker()
      : BT(new BugType(this, "Per-CPU non-atomic RMW race", "Concurrency")) {}

  void checkASTCodeBody(const Decl *D, AnalysisManager &Mgr,
                        BugReporter &BR) const;

  void checkEndAnalysis(ExplodedGraph &G, BugReporter &BR,
                        ExprEngine &Eng) const;

private:
  // Utilities used by the AST traversal.
  static const VarDecl *getReferencedVar(const Expr *E) {
    if (!E)
      return nullptr;
    E = E->IgnoreParenCasts();
    if (const auto *DRE = dyn_cast<DeclRefExpr>(E)) {
      return dyn_cast<VarDecl>(DRE->getDecl());
    }
    return nullptr;
  }

  static const MemberExpr *asMemberExpr(const Expr *E) {
    if (!E)
      return nullptr;
    return dyn_cast<MemberExpr>(E->IgnoreParenCasts());
  }

  static const FieldDecl *getFieldFromMemberExpr(const MemberExpr *ME) {
    if (!ME)
      return nullptr;
    return dyn_cast<FieldDecl>(ME->getMemberDecl());
  }

  static const VarDecl *getBaseVarFromMemberExpr(const MemberExpr *ME) {
    if (!ME)
      return nullptr;
    const Expr *Base = ME->getBase()->IgnoreParenCasts();
    return getReferencedVar(Base);
  }

  static std::string buildFieldKey(const MemberExpr *ME) {
    if (!ME)
      return std::string();
    const FieldDecl *FD = getFieldFromMemberExpr(ME);
    if (!FD)
      return std::string();

    // Get record name from the base type's pointee (for ->) or directly (for .)
    QualType BaseQT;
    const Expr *Base = ME->getBase()->IgnoreParenCasts();
    if (ME->isArrow())
      BaseQT = Base->getType()->getPointeeType();
    else
      BaseQT = Base->getType();

    if (BaseQT.isNull())
      return FD->getNameAsString(); // fallback

    const RecordType *RT = BaseQT->getAs<RecordType>();
    std::string RecName;
    if (RT && RT->getDecl())
      RecName = RT->getDecl()->getNameAsString();

    if (RecName.empty())
      return FD->getNameAsString();

    return RecName + "." + FD->getNameAsString();
  }

  static bool isCallNamed(const Expr *E, StringRef Name) {
    if (!E)
      return false;
    const auto *CE = dyn_cast<CallExpr>(E->IgnoreParenCasts());
    if (!CE)
      return false;
    if (const FunctionDecl *FD = CE->getDirectCallee()) {
      return FD->getName() == Name;
    }
    // Fallback: try if callee expression is a DeclRefExpr
    const Expr *Callee = CE->getCallee()->IgnoreParenCasts();
    if (const auto *DRE = dyn_cast<DeclRefExpr>(Callee)) {
      if (const auto *FD2 = dyn_cast<FunctionDecl>(DRE->getDecl()))
        return FD2->getName() == Name;
    }
    return false;
    }

  static bool rhsReadsSameField(const Expr *RHS, const VarDecl *BaseVar,
                                const FieldDecl *Field) {
    if (!RHS || !BaseVar || !Field)
      return false;

    // Recursive search in RHS for a MemberExpr that refers to the same
    // base VarDecl and same FieldDecl.
    struct LocalVisitor : public RecursiveASTVisitor<LocalVisitor> {
      const VarDecl *BaseVar;
      const FieldDecl *Field;
      bool Found = false;

      explicit LocalVisitor(const VarDecl *BV, const FieldDecl *F)
          : BaseVar(BV), Field(F) {}

      bool VisitMemberExpr(MemberExpr *ME) {
        if (Found)
          return true;
        const FieldDecl *FD = dyn_cast<FieldDecl>(ME->getMemberDecl());
        if (!FD)
          return true;
        if (FD != Field)
          return true;
        const VarDecl *BV = nullptr;
        const Expr *Base = ME->getBase();
        if (Base)
          BV = SAGenTestChecker::getReferencedVar(Base);
        if (BV == BaseVar) {
          Found = true;
        }
        return true;
      }
    };

    LocalVisitor V(BaseVar, Field);
    V.TraverseStmt(const_cast<Expr *>(RHS));
    return V.Found;
  }

  // Visitor for a single function body.
  class FuncVisitor : public RecursiveASTVisitor<FuncVisitor> {
    ASTContext &Ctx;
    // Local per-function mapping from variables to percpu kind.
    llvm::DenseMap<const VarDecl *, PerCpuKind> VarKinds;

    // References to the checker's global site maps.
    std::map<std::string, SiteList> &LocalRMWSites;
    std::map<std::string, SiteList> &RemoteWriteSites;

  public:
    FuncVisitor(ASTContext &Ctx,
                std::map<std::string, SiteList> &LocalRMW,
                std::map<std::string, SiteList> &RemoteWrites)
        : Ctx(Ctx), LocalRMWSites(LocalRMW), RemoteWriteSites(RemoteWrites) {}

    bool VisitVarDecl(VarDecl *VD) {
      if (!VD || !VD->hasInit())
        return true;
      const Expr *Init = VD->getInit();
      handleInitOrAssignmentToVar(VD, Init);
      return true;
    }

    bool VisitBinaryOperator(BinaryOperator *BO) {
      if (!BO)
        return true;
      if (BO->getOpcode() == BO_Assign) {
        const Expr *LHS = BO->getLHS();
        const Expr *RHS = BO->getRHS();

        // Alias propagation: LHS is a variable, RHS is variable or call
        if (const VarDecl *LHSVar = SAGenTestChecker::getReferencedVar(LHS)) {
          handleInitOrAssignmentToVar(LHSVar, RHS);
        }

        // Remote write detection: LHS member through RemoteCPU pointer var
        if (const MemberExpr *ME = SAGenTestChecker::asMemberExpr(LHS)) {
          const VarDecl *BaseVar = SAGenTestChecker::getBaseVarFromMemberExpr(ME);
          if (BaseVar) {
            auto It = VarKinds.find(BaseVar);
            if (It != VarKinds.end() && It->second == PerCpuKind::RemoteCPU) {
              std::string Key = SAGenTestChecker::buildFieldKey(ME);
              if (!Key.empty())
                RemoteWriteSites[Key].push_back(BO);
            }
          }
        }

        // Local explicit RMW: LHS is member via ThisCPU var and RHS reads same field
        if (const MemberExpr *ME = SAGenTestChecker::asMemberExpr(LHS)) {
          const VarDecl *BaseVar = SAGenTestChecker::getBaseVarFromMemberExpr(ME);
          const FieldDecl *Field = SAGenTestChecker::getFieldFromMemberExpr(ME);
          if (BaseVar && Field) {
            auto It = VarKinds.find(BaseVar);
            if (It != VarKinds.end() && It->second == PerCpuKind::ThisCPU) {
              if (SAGenTestChecker::rhsReadsSameField(RHS, BaseVar, Field)) {
                std::string Key = SAGenTestChecker::buildFieldKey(ME);
                if (!Key.empty())
                  LocalRMWSites[Key].push_back(BO);
              }
            }
          }
        }
      }
      return true;
    }

    bool VisitCompoundAssignOperator(CompoundAssignOperator *CAO) {
      if (!CAO)
        return true;
      const Expr *LHS = CAO->getLHS();
      if (const MemberExpr *ME = SAGenTestChecker::asMemberExpr(LHS)) {
        const VarDecl *BaseVar = SAGenTestChecker::getBaseVarFromMemberExpr(ME);
        if (BaseVar) {
          auto It = VarKinds.find(BaseVar);
          if (It != VarKinds.end() && It->second == PerCpuKind::ThisCPU) {
            std::string Key = SAGenTestChecker::buildFieldKey(ME);
            if (!Key.empty())
              LocalRMWSites[Key].push_back(CAO);
          }
        }
      }
      return true;
    }

  private:
    // Scanner for per-CPU pointer derivations inside GNU statement
    // expressions: this_cpu_ptr/per_cpu_ptr are macros in the kernel tree,
    // so classification must look at how the per-CPU offset is computed.
    // A current-CPU derivation uses segment-relative inline asm or an
    // offset lookup whose index is itself computed by a call; an
    // explicit-CPU derivation indexes a per-CPU offset table with a plain
    // value.
    class PercpuRhsScanner
        : public RecursiveASTVisitor<PercpuRhsScanner> {
    public:
      bool HasAsm = false;
      bool HasCurrentCpuEvidence = false;
      bool HasOffsetTableIndex = false;
      bool SubscriptIndexHasCall = false;

      bool VisitAsmStmt(AsmStmt *S) {
        (void)S;
        HasAsm = true;
        return true;
      }

      bool VisitArraySubscriptExpr(ArraySubscriptExpr *ASE) {
        if (!ASE)
          return true;
        const Expr *Base = ASE->getBase();
        if (Base)
          Base = Base->IgnoreParenImpCasts();
        if (const auto *DRE = dyn_cast<DeclRefExpr>(Base)) {
          if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
            if (VD->getType()->isArrayType()) {
              HasOffsetTableIndex = true;
              if (containsAnyCall(ASE->getIdx()))
                SubscriptIndexHasCall = true;
            }
          }
        }
        return true;
      }

      bool VisitDeclRefExpr(DeclRefExpr *DRE) {
        if (!DRE)
          return true;
        if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
          StringRef N = VD->getName();
          if (N == "this_cpu_off" || N == "__my_cpu_offset" ||
              N == "my_cpu_offset")
            HasCurrentCpuEvidence = true;
          else if (N == "__per_cpu_offset")
            HasOffsetTableIndex = true;
        }
        return true;
      }

      bool VisitCallExpr(CallExpr *CE) {
        if (!CE)
          return true;
        const FunctionDecl *FD = CE->getDirectCallee();
        if (!FD && CE->getCallee()) {
          const Expr *Callee = CE->getCallee()->IgnoreParenImpCasts();
          if (const auto *DRE = dyn_cast<DeclRefExpr>(Callee))
            FD = dyn_cast<FunctionDecl>(DRE->getDecl());
        }
        if (FD) {
          StringRef N = FD->getName();
          if (N == "this_cpu_ptr" || N == "raw_cpu_ptr" ||
              N == "smp_processor_id" || N == "raw_smp_processor_id" ||
              N == "debug_smp_processor_id")
            HasCurrentCpuEvidence = true;
          else if (N == "per_cpu_ptr")
            HasOffsetTableIndex = true;
        }
        return true;
      }

      static bool containsAnyCall(const Expr *E) {
        if (!E)
          return false;
        struct CallFinder : public RecursiveASTVisitor<CallFinder> {
          bool Found = false;
          bool VisitCallExpr(CallExpr *CE) {
            (void)CE;
            Found = true;
            return false;
          }
        };
        CallFinder F;
        F.TraverseStmt(const_cast<Expr *>(E));
        return F.Found;
      }
    };

    // Classify a per-CPU pointer derivation from an initializer or
    // assignment RHS. Direct calls still work; otherwise the macro
    // expansion (GNU statement expression) is inspected.
    PerCpuKind classifyPercpuRHS(const Expr *RHS) const {
      if (!RHS)
        return PerCpuKind::Unknown;

      if (isCallNamed(RHS, "this_cpu_ptr") || isCallNamed(RHS, "raw_cpu_ptr"))
        return PerCpuKind::ThisCPU;
      if (isCallNamed(RHS, "per_cpu_ptr"))
        return PerCpuKind::RemoteCPU;

      const auto *SE = dyn_cast<StmtExpr>(RHS->IgnoreParenImpCasts());
      if (!SE)
        return PerCpuKind::Unknown;

      PercpuRhsScanner Scan;
      Scan.TraverseStmt(const_cast<Stmt *>(static_cast<const Stmt *>(SE)));

      if (Scan.HasAsm || Scan.HasCurrentCpuEvidence ||
          Scan.SubscriptIndexHasCall)
        return PerCpuKind::ThisCPU;
      if (Scan.HasOffsetTableIndex)
        return PerCpuKind::RemoteCPU;
      return PerCpuKind::Unknown;
    }

    void handleInitOrAssignmentToVar(const VarDecl *TargetVD, const Expr *RHS) {
      if (!TargetVD || !RHS)
        return;

      PerCpuKind Kind = classifyPercpuRHS(RHS);
      if (Kind != PerCpuKind::Unknown) {
        VarKinds[TargetVD] = Kind;
        return;
      }

      // Alias propagation if RHS references another variable
      if (const VarDecl *RHSVar = SAGenTestChecker::getReferencedVar(RHS)) {
        auto It = VarKinds.find(RHSVar);
        if (It != VarKinds.end() && It->second != PerCpuKind::Unknown) {
          VarKinds[TargetVD] = It->second;
        }
      }
    }
  };
};

void SAGenTestChecker::checkASTCodeBody(const Decl *D, AnalysisManager &Mgr,
                                        BugReporter &BR) const {
  if (!D)
    return;
  const Stmt *Body = D->getBody();
  if (!Body)
    return;

  // Perform a per-function AST traversal and collect sites into the
  // checker-level maps (LocalRMWSites and RemoteWriteSites).
  ASTContext &Ctx = Mgr.getASTContext();
  FuncVisitor V(Ctx,
                const_cast<std::map<std::string, SiteList> &>(LocalRMWSites),
                const_cast<std::map<std::string, SiteList> &>(RemoteWriteSites));
  V.TraverseStmt(const_cast<Stmt *>(Body));
}

void SAGenTestChecker::checkEndAnalysis(ExplodedGraph &G, BugReporter &BR,
                                        ExprEngine &Eng) const {
  if (LocalRMWSites.empty() || RemoteWriteSites.empty())
    return;

  // For each field that has a local non-atomic RMW and also has a remote write,
  // report the local RMW sites and add a note pointing to one remote write.
  for (const auto &LR : LocalRMWSites) {
    const std::string &Key = LR.first;
    auto RWIt = RemoteWriteSites.find(Key);
    if (RWIt == RemoteWriteSites.end())
      continue;

    const SiteList &LocalSites = LR.second;
    const SiteList &RemoteSites = RWIt->second;
    if (RemoteSites.empty())
      continue;

    // Representative remote location for note.
    const Stmt *RemoteS = RemoteSites.front();
    PathDiagnosticLocation RemoteLoc =
        PathDiagnosticLocation::createBegin(RemoteS, BR.getSourceManager(),
                                            nullptr);

    for (const Stmt *S : LocalSites) {
      if (!S)
        continue;
      PathDiagnosticLocation Loc =
          PathDiagnosticLocation::createBegin(S, BR.getSourceManager(),
                                              nullptr);

      auto R = std::make_unique<BasicBugReport>(
          *BT,
          "Non-atomic RMW on per-CPU field raced with remote write; wrap with READ_ONCE/WRITE_ONCE.",
          Loc);
      R->addRange(S->getSourceRange());
      R->addNote("Remote write occurs here", RemoteLoc);
      BR.emitReport(std::move(R));
    }
  }
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detect non-atomic per-CPU RMW raced with remote per_cpu_ptr write; suggest READ_ONCE/WRITE_ONCE.",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
