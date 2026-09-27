#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Checkers/Taint.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/Environment.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SymExpr.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtVisitor.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/StaticAnalyzer/Checkers/utility.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Decl.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"
#include <string>
#include <memory>
#include <functional>

using namespace clang;
using namespace ento;
using namespace taint;

namespace {

struct FreedTarget {
  std::string LabelName;
  const CallExpr *FreeCallCE;     // The free call statement (for diagnostics)
  const Expr *FreedExpr;          // The expression passed to free (arg0)
  std::string CalleeName;         // Name of free function (kfree, etc.)
};

class SAGenTestChecker : public Checker<check::ASTCodeBody> {
   mutable std::unique_ptr<BugType> BT;

   public:
      SAGenTestChecker() : BT(new BugType(this, "Suspicious cleanup free in early error path", "Memory Management")) {}

      void checkASTCodeBody(const Decl *D, AnalysisManager &Mgr, BugReporter &BR) const;

   private:

      // Helpers
      static bool isKnownFreeName(StringRef Name) {
        return Name.equals("kfree") || Name.equals("kvfree") || Name.equals("vfree");
      }

      static bool isKnownAllocationName(StringRef Name) {
        return Name.equals("kmalloc") || Name.equals("kzalloc") ||
               Name.equals("kcalloc") || Name.equals("kvzalloc") ||
               Name.equals("vzalloc");
      }

      static const VarDecl *getDirectVar(const Expr *E) {
        const auto *DRE = dyn_cast_or_null<DeclRefExpr>(
            E ? E->IgnoreParenImpCasts() : nullptr);
        return DRE ? dyn_cast<VarDecl>(DRE->getDecl()) : nullptr;
      }

      static bool isAllocatedLocal(const VarDecl *VD) {
        if (!VD || !VD->hasLocalStorage() || !VD->hasInit())
          return false;
        const auto *CE = dyn_cast<CallExpr>(VD->getInit()->IgnoreParenImpCasts());
        const FunctionDecl *Callee = CE ? CE->getDirectCallee() : nullptr;
        const IdentifierInfo *II = Callee ? Callee->getIdentifier() : nullptr;
        return II && isKnownAllocationName(II->getName());
      }

      static bool cleanupFreesVar(
          const llvm::SmallVector<FreedTarget, 4> &Targets,
          const VarDecl *VD) {
        for (const FreedTarget &Target : Targets)
          if (getDirectVar(Target.FreedExpr) == VD)
            return true;
        return false;
      }

      static const CallExpr *findFirstCallIn(const Stmt *S) {
        if (!S) return nullptr;
        if (const auto *CE = dyn_cast<CallExpr>(S))
          return CE;
        for (const Stmt *Child : S->children()) {
          if (!Child) continue;
          if (const CallExpr *Found = findFirstCallIn(Child))
            return Found;
        }
        return nullptr;
      }

      static void collectAllCallsIn(const Stmt *S, llvm::SmallVectorImpl<const CallExpr*> &Out) {
        if (!S) return;
        if (const auto *CE = dyn_cast<CallExpr>(S))
          Out.push_back(CE);
        for (const Stmt *Child : S->children()) {
          if (!Child) continue;
          collectAllCallsIn(Child, Out);
        }
      }

      static const GotoStmt *findFirstGotoIn(const Stmt *S) {
        if (!S) return nullptr;
        if (const auto *GS = dyn_cast<GotoStmt>(S))
          return GS;
        for (const Stmt *Child : S->children()) {
          if (!Child) continue;
          if (const GotoStmt *Found = findFirstGotoIn(Child))
            return Found;
        }
        return nullptr;
      }

      static const Expr *getFreedExprFromCall(const CallExpr *CE) {
        if (!CE || CE->getNumArgs() == 0) return nullptr;
        return CE->getArg(0)->IgnoreParenImpCasts();
      }

      static StringRef getLabelName(const LabelStmt *LS) {
        if (!LS) return StringRef();
        if (const LabelDecl *LD = LS->getDecl()) {
          if (const IdentifierInfo *II = LD->getIdentifier())
            return II->getName();
        }
        return StringRef();
      }

      static StringRef getLabelNameFromGoto(const GotoStmt *GS) {
        if (!GS) return StringRef();
        if (const LabelDecl *LD = GS->getLabel()) {
          if (const IdentifierInfo *II = LD->getIdentifier())
            return II->getName();
        }
        return StringRef();
      }

      static bool getEnclosingCompoundAndIndex(const Stmt *S, ASTContext &Ctx,
                                               const CompoundStmt *&OutCS, unsigned &OutIdx) {
        if (!S) return false;
        ParentMapContext &PMC = Ctx.getParentMapContext();
        const Stmt *Cur = S;
        // Limit the search depth to avoid pathological cases.
        for (int Depth = 0; Depth < 64 && Cur; ++Depth) {
          auto Parents = PMC.getParents(*Cur);
          if (Parents.empty())
            return false;
          const Stmt *Next = nullptr;
          for (const auto &P : Parents) {
            if (const auto *CS = P.get<CompoundStmt>()) {
              // Find the index of Cur in CS
              unsigned I = 0;
              for (const Stmt *Child : CS->body()) {
                if (Child == Cur) {
                  OutCS = CS;
                  OutIdx = I;
                  return true;
                }
                ++I;
              }
              // Even if parent is CS but Cur isn't direct child (shouldn't happen), continue.
            }
            if (const auto *PS = P.get<Stmt>()) {
              Next = PS;
              // Keep searching upwards until we hit a CompoundStmt that directly contains Cur.
            } else {
              // Parent is not a Stmt, likely a Decl - stop this branch.
            }
          }
          Cur = Next;
        }
        return false;
      }

      static const CallExpr *getCallFromAssignment(const Stmt *S) {
        if (!S) return nullptr;
        const auto *BO = dyn_cast<BinaryOperator>(S);
        if (!BO || !BO->isAssignmentOp())
          return nullptr;
        const Expr *RHS = BO->getRHS();
        return dyn_cast_or_null<CallExpr>(RHS ? RHS->IgnoreParenImpCasts() : nullptr);
      }

      static const CallExpr *getCallFromDeclInit(const Stmt *S) {
        const auto *DS = dyn_cast<DeclStmt>(S);
        if (!DS) return nullptr;
        for (const Decl *Di : DS->decls()) {
          if (const auto *VD = dyn_cast<VarDecl>(Di)) {
            if (const Expr *Init = VD->getInit()) {
              if (const auto *CE = dyn_cast<CallExpr>(Init->IgnoreParenImpCasts()))
                return CE;
            }
          }
        }
        return nullptr;
      }

      static const CallExpr *findCallBeforeIf(const IfStmt *IfS, ASTContext &Ctx) {
        if (!IfS) return nullptr;
        // Case 1: condition is a call
        if (const Expr *Cond = IfS->getCond()) {
          if (const auto *CE = dyn_cast<CallExpr>(Cond->IgnoreParenImpCasts()))
            return CE;
        }

        // Case 2: previous sibling contains the call (assignment or decl-init)
        const CompoundStmt *CS = nullptr;
        unsigned Idx = 0;
        if (!getEnclosingCompoundAndIndex(IfS, Ctx, CS, Idx) || !CS)
          return nullptr;

        if (Idx == 0)
          return nullptr;

        // Find the nearest preceding status-producing assignment or initializer;
        // unrelated statements between it and the guard do not change the relation.
        for (unsigned I = Idx; I > 0; --I) {
          const Stmt *Prev = getStmtAtIndex(CS, I - 1);
          if (!Prev) continue;
          if (const CallExpr *CE = getCallFromAssignment(Prev))
            return CE;
          if (const CallExpr *CE = getCallFromDeclInit(Prev))
            return CE;
        }

        const Stmt *Prev = getStmtAtIndex(CS, Idx - 1);
        if (!Prev) return nullptr;
        if (const auto *CE = dyn_cast<CallExpr>(Prev))
          return CE;
        return findFirstCallIn(Prev);
      }

      static void collectStructPtrArgs(const CallExpr *CE,
                                       llvm::SmallVectorImpl<const VarDecl*> &Out) {
        if (!CE) return;
        for (unsigned i = 0; i < CE->getNumArgs(); ++i) {
          const Expr *Arg = CE->getArg(i)->IgnoreParenImpCasts();
          const auto *DRE = dyn_cast<DeclRefExpr>(Arg);
          if (!DRE) continue;
          const auto *VD = dyn_cast<VarDecl>(DRE->getDecl());
          if (!VD) continue;
          QualType T = DRE->getType();
          if (const auto *PT = T->getAs<PointerType>()) {
            QualType Pointee = PT->getPointeeType();
            if (Pointee->getAs<RecordType>()) {
              Out.push_back(VD);
            }
          }
        }
      }

      static bool isMemberOfVar(const Expr *E, const VarDecl *V, std::string &FieldNameOut) {
        if (!E) return false;
        const auto *ME = dyn_cast<MemberExpr>(E->IgnoreParenImpCasts());
        if (!ME) return false;
        const Expr *Base = ME->getBase();
        if (!Base) return false;
        const auto *DRE = dyn_cast<DeclRefExpr>(Base->IgnoreParenImpCasts());
        if (!DRE) return false;
        const auto *BVD = dyn_cast<VarDecl>(DRE->getDecl());
        if (!BVD || BVD != V) return false;
        if (const auto *FD = dyn_cast<FieldDecl>(ME->getMemberDecl())) {
          FieldNameOut = FD->getNameAsString();
          return true;
        }
        if (const auto *ND = dyn_cast<NamedDecl>(ME->getMemberDecl())) {
          FieldNameOut = ND->getNameAsString();
          return true;
        }
        return false;
      }

      static bool calleeFreesArgumentMember(const CallExpr *Call,
                                            const VarDecl *CallerObject,
                                            const FieldDecl *Field) {
        const FunctionDecl *Callee = Call ? Call->getDirectCallee() : nullptr;
        if (!Callee || !Callee->hasBody() || !Field)
          return false;

        unsigned ArgIndex = 0;
        for (const ParmVarDecl *Param : Callee->parameters()) {
          if (ArgIndex >= Call->getNumArgs())
            break;
          if (getDirectVar(Call->getArg(ArgIndex)) == CallerObject) {
            llvm::SmallVector<const CallExpr *, 8> Calls;
            collectAllCallsIn(Callee->getBody(), Calls);
            for (const CallExpr *FreeCall : Calls) {
              const FunctionDecl *FreeCallee = FreeCall->getDirectCallee();
              const IdentifierInfo *II = FreeCallee ? FreeCallee->getIdentifier() : nullptr;
              if (!II || !isKnownFreeName(II->getName()) ||
                  FreeCall->getNumArgs() == 0)
                continue;
              const auto *Member = dyn_cast<MemberExpr>(
                  FreeCall->getArg(0)->IgnoreParenImpCasts());
              const auto *BaseRef = Member ? dyn_cast<DeclRefExpr>(
                  Member->getBase()->IgnoreParenImpCasts()) : nullptr;
              const auto *BaseParam = BaseRef ? dyn_cast<ParmVarDecl>(BaseRef->getDecl()) : nullptr;
              if (BaseParam == Param && Member->getMemberDecl() == Field)
                return true;
            }
          }
          ++ArgIndex;
        }
        return false;
      }

      static bool stmtContainsAssignmentToMember(const Stmt *S,
                                                 const VarDecl *Base,
                                                 StringRef Field) {
        if (!S) return false;

        // Check if S is an assignment to the target member
        if (const auto *BO = dyn_cast<BinaryOperator>(S)) {
          if (BO->isAssignmentOp()) {
            const Expr *LHS = BO->getLHS();
            const auto *ME = dyn_cast<MemberExpr>(LHS ? LHS->IgnoreParenImpCasts() : nullptr);
            if (ME) {
              const Expr *BaseE = ME->getBase();
              const auto *DRE = dyn_cast<DeclRefExpr>(BaseE ? BaseE->IgnoreParenImpCasts() : nullptr);
              const auto *BVD = DRE ? dyn_cast<VarDecl>(DRE->getDecl()) : nullptr;
              if (BVD == Base) {
                std::string Name;
                if (const auto *FD = dyn_cast<FieldDecl>(ME->getMemberDecl()))
                  Name = FD->getNameAsString();
                else if (const auto *ND = dyn_cast<NamedDecl>(ME->getMemberDecl()))
                  Name = ND->getNameAsString();
                if (!Name.empty() && Field.equals(Name))
                  return true;
              }
            }
          }
        }

        // Recurse into children
        for (const Stmt *Child : S->children()) {
          if (!Child) continue;
          if (stmtContainsAssignmentToMember(Child, Base, Field))
            return true;
        }
        return false;
      }

      // Helper to get the number of statements in a CompoundStmt in a version-agnostic way.
      static unsigned getCompoundBodySize(const CompoundStmt *CS) {
        unsigned N = 0;
        for (const Stmt *Child : CS->body()) {
          (void)Child;
          ++N;
        }
        return N;
      }

      // Helper to get the statement at a given index in a CompoundStmt.
      static const Stmt *getStmtAtIndex(const CompoundStmt *CS, unsigned Index) {
        unsigned I = 0;
        for (const Stmt *Child : CS->body()) {
          if (I == Index)
            return Child;
          ++I;
        }
        return nullptr;
      }

      static void buildCleanupMap(const Stmt *Body, ASTContext &Ctx,
                                  llvm::StringMap<llvm::SmallVector<FreedTarget, 4>> &OutMap) {
        if (!Body) return;

        // Traverse the body to find all LabelStmt
        llvm::SmallVector<const LabelStmt*, 16> Labels;
        // Collect labels
        std::function<void(const Stmt*)> CollectLabels = [&](const Stmt *S){
          if (!S) return;
          if (const auto *LS = dyn_cast<LabelStmt>(S))
            Labels.push_back(LS);
          for (const Stmt *Child : S->children()) {
            if (Child) CollectLabels(Child);
          }
        };
        CollectLabels(Body);

        // For each label, scan forward in the enclosing compound to collect free-like calls
        for (const LabelStmt *LS : Labels) {
          StringRef LName = getLabelName(LS);
          if (LName.empty()) continue;

          const CompoundStmt *CS = nullptr;
          unsigned Idx = 0;
          if (!getEnclosingCompoundAndIndex(LS, Ctx, CS, Idx) || !CS)
            continue;

          // Scan forward from the statement just after the LabelStmt
          unsigned CSSize = getCompoundBodySize(CS);
          for (unsigned I = Idx + 1; I < CSSize; ++I) {
            const Stmt *Cur = getStmtAtIndex(CS, I);
            if (!Cur) continue;

            if (isa<LabelStmt>(Cur)) {
              // Another label signals end of this cleanup region
              break;
            }

            // Collect known free calls within this statement
            llvm::SmallVector<const CallExpr*, 8> Calls;
            collectAllCallsIn(Cur, Calls);
            for (const CallExpr *CE : Calls) {
              const FunctionDecl *FD = CE->getDirectCallee();
              if (!FD) continue;
              const IdentifierInfo *II = FD->getIdentifier();
              if (!II) continue;
              StringRef Name = II->getName();
              if (!isKnownFreeName(Name)) continue;

              const Expr *Arg0 = getFreedExprFromCall(CE);
              if (!Arg0) continue;

              FreedTarget FT;
              FT.LabelName = LName.str();
              FT.FreeCallCE = CE;
              FT.FreedExpr = Arg0;
              FT.CalleeName = Name.str();
              OutMap[LName].push_back(FT);
            }

            // Stop at control-flow ending constructs, typical for cleanup regions
            if (isa<ReturnStmt>(Cur) || isa<GotoStmt>(Cur) || isa<BreakStmt>(Cur) || isa<ContinueStmt>(Cur))
              break;
          }
        }
      }

      static void collectIfStmts(const Stmt *Body, llvm::SmallVectorImpl<const IfStmt*> &Out) {
        if (!Body) return;
        if (const auto *IS = dyn_cast<IfStmt>(Body))
          Out.push_back(IS);
        for (const Stmt *Child : Body->children()) {
          if (!Child) continue;
          collectIfStmts(Child, Out);
        }
      }

      static bool sawPriorLocalAssignmentTo(const IfStmt *IfS, ASTContext &Ctx,
                                            const VarDecl *Base, StringRef Field) {
        if (!IfS || !Base) return false;
        const CompoundStmt *CS = nullptr;
        unsigned IfIdx = 0;
        if (!getEnclosingCompoundAndIndex(IfS, Ctx, CS, IfIdx) || !CS)
          return false;

        // Search in siblings before IfS
        for (unsigned I = 0; I < IfIdx; ++I) {
          const Stmt *Cur = getStmtAtIndex(CS, I);
          if (!Cur) continue;
          if (stmtContainsAssignmentToMember(Cur, Base, Field))
            return true;
        }
        return false;
      }
};

void SAGenTestChecker::checkASTCodeBody(const Decl *D, AnalysisManager &Mgr, BugReporter &BR) const {
  const auto *FD = dyn_cast<FunctionDecl>(D);
  if (!FD) return;
  const Stmt *Body = FD->getBody();
  if (!Body) return;

  ASTContext &Ctx = BR.getContext();

  // Step B: Build label -> cleanup free targets map
  llvm::StringMap<llvm::SmallVector<FreedTarget, 4>> CleanupMap;
  buildCleanupMap(Body, Ctx, CleanupMap);

  if (CleanupMap.empty())
    return;

  // Step C: Find IfStmts with early goto in then-branch and calls right before them
  llvm::SmallVector<const IfStmt*, 32> Ifs;
  collectIfStmts(Body, Ifs);

  AnalysisDeclContext *ADC = Mgr.getAnalysisDeclContext(D);

  for (const IfStmt *IfS : Ifs) {
    const Stmt *Then = IfS->getThen();
    if (!Then) continue;

    const GotoStmt *GS = findFirstGotoIn(Then);
    if (!GS) continue;

    StringRef TargetLabel = getLabelNameFromGoto(GS);
    if (TargetLabel.empty()) continue;

    auto It = CleanupMap.find(TargetLabel);
    if (It == CleanupMap.end()) continue;

    const CallExpr *FailingCall = findCallBeforeIf(IfS, Ctx);
    if (!FailingCall) continue;

    // An allocated argument may be an output buffer whose ownership must be
    // released on this failure edge. Warn when this cleanup omits it but a
    // later cleanup region releases the same local allocation.
    llvm::SmallVector<const VarDecl*, 8> StructArgs;
    collectStructPtrArgs(FailingCall, StructArgs);
    for (unsigned ArgIndex = 0; ArgIndex < FailingCall->getNumArgs(); ++ArgIndex) {
      const VarDecl *VD = getDirectVar(FailingCall->getArg(ArgIndex));
      if (!isAllocatedLocal(VD) || cleanupFreesVar(It->second, VD))
        continue;

      bool ReleasedByLaterCleanup = false;
      for (const auto &Entry : CleanupMap) {
        if (cleanupFreesVar(Entry.second, VD) &&
            Entry.second.front().FreeCallCE->getBeginLoc() > GS->getBeginLoc()) {
          ReleasedByLaterCleanup = true;
          break;
        }
      }
      if (!ReleasedByLaterCleanup)
        continue;

      PathDiagnosticLocation Loc = PathDiagnosticLocation::createBegin(
          FailingCall, BR.getSourceManager(), ADC);
      auto R = std::make_unique<BasicBugReport>(
          *BT,
          "Failure cleanup skips releasing an allocated argument that a later cleanup region frees",
          Loc);
      R->addRange(FailingCall->getSourceRange());
      BR.emitReport(std::move(R));
    }
    if (StructArgs.empty()) continue;

    // Step D: For each free in the cleanup region, see if it's freeing a member of any struct arg
    for (const FreedTarget &FT : It->second) {
      const Expr *E = FT.FreedExpr;
      const auto *ME = dyn_cast<MemberExpr>(E ? E->IgnoreParenImpCasts() : nullptr);
      if (!ME) continue;

      const Expr *Base = ME->getBase();
      const auto *DRE = dyn_cast<DeclRefExpr>(Base ? Base->IgnoreParenImpCasts() : nullptr);
      if (!DRE) continue;

      const auto *BVD = dyn_cast<VarDecl>(DRE->getDecl());
      if (!BVD) continue;

      // Check if Base is among failing call's struct pointer args
      bool IsStructArg = llvm::is_contained(StructArgs, BVD);
      if (!IsStructArg) continue;

      // The duplicate-free trigger requires the failing callee to release this
      // same member through the corresponding struct-pointer argument.
      const auto *Field = dyn_cast<FieldDecl>(ME->getMemberDecl());
      if (!Field || !calleeFreesArgumentMember(FailingCall, BVD, Field))
        continue;

      // Step E: Report
      PathDiagnosticLocation Loc = PathDiagnosticLocation::createBegin(FT.FreeCallCE, BR.getSourceManager(), ADC);
      auto R = std::make_unique<BasicBugReport>(
          *BT,
          "Suspicious cleanup free of a struct member after a failing operation; ownership may already have been released",
          Loc);
      R->addRange(FT.FreeCallCE->getSourceRange());
      BR.emitReport(std::move(R));
    }
  }
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects goto-based early error cleanup that frees struct fields not initialized in the current function",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
