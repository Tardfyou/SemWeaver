#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtVisitor.h"
#include "llvm/Support/raw_ostream.h"
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

// No custom program state for this checker.

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

      // The variable whose value the guard tests, unwrapping negation and casts.
      static const VarDecl *getGuardVar(const IfStmt *IfS) {
        if (!IfS) return nullptr;
        const Expr *Cond = IfS->getCond();
        if (!Cond) return nullptr;
        Cond = Cond->IgnoreParenImpCasts();
        if (const auto *UO = dyn_cast<UnaryOperator>(Cond)) {
          if (UO->getOpcode() == UO_LNot)
            Cond = UO->getSubExpr()->IgnoreParenImpCasts();
        }
        if (const auto *DRE = dyn_cast<DeclRefExpr>(Cond))
          if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl()))
            return VD;
        return nullptr;
      }

      // True if S is a direct assignment to V (base variable, not a member).
      static bool assignsVar(const Stmt *S, const VarDecl *V) {
        const auto *BO = dyn_cast<BinaryOperator>(S);
        if (!BO || !BO->isAssignmentOp()) return false;
        const Expr *LHS = BO->getLHS();
        const auto *DRE = dyn_cast_or_null<DeclRefExpr>(LHS ? LHS->IgnoreParenImpCasts() : nullptr);
        return DRE && DRE->getDecl() == V;
      }

      static const CallExpr *findCallBeforeIf(const IfStmt *IfS, ASTContext &Ctx) {
        if (!IfS) return nullptr;
        // Case 1: condition is a call
        if (const Expr *Cond = IfS->getCond()) {
          if (const auto *CE = dyn_cast<CallExpr>(Cond->IgnoreParenImpCasts()))
            return CE;
        }

        const CompoundStmt *CS = nullptr;
        unsigned Idx = 0;
        if (!getEnclosingCompoundAndIndex(IfS, Ctx, CS, Idx) || !CS)
          return nullptr;

        // Case 2: bind the guard's tested variable to the call that produced
        // it. Scanning backwards over unrelated statements keeps this a value
        // relation between the status variable and the failing call rather
        // than mere statement adjacency.
        const VarDecl *GuardVar = getGuardVar(IfS);
        const unsigned LookbackLimit = 8;
        unsigned Scanned = 0;
        for (unsigned I = Idx; I-- > 0 && Scanned < LookbackLimit; ++Scanned) {
          const Stmt *Prev = getStmtAtIndex(CS, I);
          if (!Prev) continue;
          if (isa<LabelStmt>(Prev))
            break; // left the straight-line region guarded by this condition

          if (GuardVar) {
            if (assignsVar(Prev, GuardVar)) {
              // The status variable is rebound here; only a call RHS keeps
              // the guard/failing-call relation alive.
              return getCallFromAssignment(Prev);
            }
            if (const auto *DS = dyn_cast<DeclStmt>(Prev)) {
              for (const Decl *Di : DS->decls()) {
                const auto *VD = dyn_cast<VarDecl>(Di);
                if (!VD || VD != GuardVar) continue;
                if (const Expr *Init = VD->getInit()) {
                  if (const auto *CE = dyn_cast<CallExpr>(Init->IgnoreParenImpCasts()))
                    return CE;
                }
              }
            }
            continue; // unrelated statement; keep looking for the producer
          }

          if (const CallExpr *CE = getCallFromAssignment(Prev))
            return CE;
          if (const CallExpr *CE = getCallFromDeclInit(Prev))
            return CE;
          if (const auto *CE = dyn_cast<CallExpr>(Prev))
            return CE;
          return findFirstCallIn(Prev);
        }
        return nullptr;
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

      // Locals of Body whose value is a copy of Var->Field: the value
      // relation through which a callee can release the received field
      // under a local alias name instead of the spelling 'param->field'.
      static void collectFieldAliases(const Stmt *Body, const VarDecl *Var,
                                      StringRef Field,
                                      llvm::SmallVectorImpl<const VarDecl*> &Out) {
        if (!Body) return;
        if (const auto *DS = dyn_cast<DeclStmt>(Body)) {
          for (const Decl *Di : DS->decls()) {
            const auto *VD = dyn_cast<VarDecl>(Di);
            if (!VD || !VD->getInit()) continue;
            std::string FName;
            if (isMemberOfVar(VD->getInit()->IgnoreParenImpCasts(), Var,
                              FName) &&
                Field.equals(FName))
              Out.push_back(VD);
          }
        }
        if (const auto *BO = dyn_cast<BinaryOperator>(Body)) {
          if (BO->isAssignmentOp() && BO->getRHS() && BO->getLHS()) {
            const auto *LHS = dyn_cast<DeclRefExpr>(
                BO->getLHS()->IgnoreParenImpCasts());
            std::string FName;
            if (LHS &&
                isMemberOfVar(BO->getRHS()->IgnoreParenImpCasts(), Var,
                              FName) &&
                Field.equals(FName)) {
              if (const auto *VD = dyn_cast<VarDecl>(LHS->getDecl()))
                Out.push_back(VD);
            }
          }
        }
        for (const Stmt *Child : Body->children()) {
          if (!Child) continue;
          collectFieldAliases(Child, Var, Field, Out);
        }
      }

      // Locals of Body whose value is a copy of Var itself.
      static void collectVarAliases(const Stmt *Body, const VarDecl *Var,
                                    llvm::SmallVectorImpl<const VarDecl*> &Out) {
        if (!Body) return;
        if (const auto *DS = dyn_cast<DeclStmt>(Body)) {
          for (const Decl *Di : DS->decls()) {
            const auto *VD = dyn_cast<VarDecl>(Di);
            if (!VD || !VD->getInit()) continue;
            const auto *DRE =
                dyn_cast<DeclRefExpr>(VD->getInit()->IgnoreParenImpCasts());
            if (DRE && DRE->getDecl() == Var)
              Out.push_back(VD);
          }
        }
        if (const auto *BO = dyn_cast<BinaryOperator>(Body)) {
          if (BO->isAssignmentOp() && BO->getRHS() && BO->getLHS()) {
            const auto *LHS = dyn_cast<DeclRefExpr>(
                BO->getLHS()->IgnoreParenImpCasts());
            const auto *RHS = dyn_cast<DeclRefExpr>(
                BO->getRHS()->IgnoreParenImpCasts());
            if (LHS && RHS && RHS->getDecl() == Var) {
              if (const auto *VD = dyn_cast<VarDecl>(LHS->getDecl()))
                Out.push_back(VD);
            }
          }
        }
        for (const Stmt *Child : Body->children()) {
          if (!Child) continue;
          collectVarAliases(Child, Var, Out);
        }
      }

      // Prove that this statement tree mutates the ownership of Var->Field
      // (FieldMode) or of the value denoted by Var (ValueMode, used once a
      // callee received Var->Field itself as an argument): the mutation may
      // be a direct free of the member, a free of a local alias of it, a
      // free inside a nested direct callee that received the struct or the
      // field value in an argument position bound to one of its parameters,
      // or an ownership-establishing store into Var->Field (e.g. a grown
      // reallocation saved back into the field). The depth bound keeps the
      // ownership proof inside the failing call's subtree.
      static bool subtreeReclaims(const Stmt *Body, const VarDecl *Var,
                                  StringRef Field, bool ValueMode, int Depth) {
        if (!Body || Depth < 0) return false;

        llvm::SmallVector<const VarDecl*, 8> Aliases;
        if (ValueMode)
          collectVarAliases(Body, Var, Aliases);
        else
          collectFieldAliases(Body, Var, Field, Aliases);

        // Ownership-establishing write: a store into Var->Field anywhere in
        // this body proves the failed call re-set the field's ownership
        // state, so a later cleanup free of the field acts on memory whose
        // ownership was last established inside the failed callee.
        if (!ValueMode && stmtContainsAssignmentToMember(Body, Var, Field))
          return true;

        llvm::SmallVector<const CallExpr*, 16> Calls;
        collectAllCallsIn(Body, Calls);
        for (const CallExpr *CE : Calls) {
          const FunctionDecl *FD = CE->getDirectCallee();
          if (!FD || !FD->getIdentifier() || CE->getNumArgs() == 0)
            continue;
          StringRef Name = FD->getIdentifier()->getName();
          const Expr *A0 = CE->getArg(0)->IgnoreParenImpCasts();

          if (isKnownFreeName(Name)) {
            std::string FName;
            if (!ValueMode && isMemberOfVar(A0, Var, FName) &&
                Field.equals(FName))
              return true; // free(Var->Field)
            if (const auto *DRE = dyn_cast<DeclRefExpr>(A0)) {
              if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
                if (ValueMode ? (VD == Var || llvm::is_contained(Aliases, VD))
                              : llvm::is_contained(Aliases, VD))
                  return true; // free of a local alias holding the value
              }
            }
          }

          if (Depth == 0) continue;
          const Stmt *NBody = FD->getBody();
          if (!NBody) continue;
          for (unsigned j = 0; j < CE->getNumArgs(); ++j) {
            if (j >= FD->getNumParams()) break;
            const Expr *Aj = CE->getArg(j)->IgnoreParenImpCasts();
            const VarDecl *NParam = FD->getParamDecl(j);

            bool CarriesStruct = false;
            bool CarriesValue = false;
            if (const auto *DRE = dyn_cast<DeclRefExpr>(Aj)) {
              if (DRE->getDecl() == Var) {
                CarriesStruct = !ValueMode;
                CarriesValue = ValueMode;
              } else if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
                CarriesValue = llvm::is_contained(Aliases, VD);
              }
            }
            if (!ValueMode && !CarriesValue) {
              std::string FName;
              if (isMemberOfVar(Aj, Var, FName) && Field.equals(FName))
                CarriesValue = true; // the field value itself is forwarded
            }

            if (CarriesStruct &&
                subtreeReclaims(NBody, NParam, Field, false, Depth - 1))
              return true;
            if (CarriesValue &&
                subtreeReclaims(NBody, NParam, Field, true, Depth - 1))
              return true;
          }
        }
        return false;
      }

      // Ownership proof for the cleanup free: the failing call's subtree must
      // mutate the ownership of the same field of the struct it received -
      // by reclaiming it (direct free, alias free, or a free inside a nested
      // direct call that received the struct or the field value) or by
      // re-establishing it (a store into the field, e.g. a reallocation
      // result saved back into it). Only then is the label-region free taken
      // after the failing call a second claim on memory whose ownership the
      // callee already handled.
      static bool calleeMutatesArgField(const CallExpr *Call, const VarDecl *Base,
                                        StringRef Field) {
        const FunctionDecl *Callee = Call ? Call->getDirectCallee() : nullptr;
        if (!Callee) return false;
        const Stmt *CalleeBody = Callee->getBody();
        if (!CalleeBody) return false;

        // Find the argument position that carries the base variable.
        int ArgIdx = -1;
        for (unsigned i = 0; i < Call->getNumArgs(); ++i) {
          const auto *DRE =
              dyn_cast<DeclRefExpr>(Call->getArg(i)->IgnoreParenImpCasts());
          if (!DRE) continue;
          const auto *VD = dyn_cast<VarDecl>(DRE->getDecl());
          if (VD && VD == Base) {
            ArgIdx = static_cast<int>(i);
            break;
          }
        }
        if (ArgIdx < 0 || ArgIdx >= static_cast<int>(Callee->getNumParams()))
          return false;
        const VarDecl *Param = Callee->getParamDecl(ArgIdx);

        return subtreeReclaims(CalleeBody, Param, Field, false, 3);
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

      // Collect the free-family calls contained in one statement into the
      // cleanup region of label LName.
      static void collectFreeTargetsInStmt(const Stmt *S, StringRef LName,
                                           llvm::StringMap<llvm::SmallVector<FreedTarget, 4>> &OutMap) {
        if (!S) return;
        llvm::SmallVector<const CallExpr*, 8> Calls;
        collectAllCallsIn(S, Calls);
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

          // The labeled statement itself is the first statement of this label's
          // cleanup region: in "free_fc: kfree(mt->fc);" the free is nested in
          // the LabelStmt rather than a following sibling, so a forward-only
          // scan would miss the free this label actually performs.
          const Stmt *Own = LS->getSubStmt();
          while (const auto *Inner = dyn_cast<LabelStmt>(Own))
            Own = Inner->getSubStmt();
          collectFreeTargetsInStmt(Own, LName, OutMap);

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
            collectFreeTargetsInStmt(Cur, LName, OutMap);

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

    // Collect struct* arguments of the failing call
    llvm::SmallVector<const VarDecl*, 8> StructArgs;
    collectStructPtrArgs(FailingCall, StructArgs);
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

      // Extract member field name
      std::string FieldName;
      if (!isMemberOfVar(E, BVD, FieldName))
        continue;

      // Ownership relation: keep the warning only when the failing callee's
      // subtree reclaims or re-establishes the same field of the struct
      // passed to it, so the cleanup free reached via this goto re-claims
      // memory whose ownership the failed callee already mutated (double
      // free). A store into the field earlier in this function does not
      // restore sole caller ownership once the failed callee re-set it.
      if (!calleeMutatesArgField(FailingCall, BVD, FieldName))
        continue; // Callee does not mutate the field; this free is legitimate

      // Step E: Report
      PathDiagnosticLocation Loc = PathDiagnosticLocation::createBegin(D, BR.getSourceManager());
      auto R = std::make_unique<BasicBugReport>(
          *BT,
          "Suspicious free of 'param->field' in early error path; may double free or free uninitialized memory",
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
