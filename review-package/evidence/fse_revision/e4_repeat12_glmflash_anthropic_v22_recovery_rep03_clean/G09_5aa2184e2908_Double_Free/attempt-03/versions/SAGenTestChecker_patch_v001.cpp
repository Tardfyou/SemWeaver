#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
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

      static const CallExpr *findCallBeforeIf(const IfStmt *IfS, ASTContext &Ctx) {
        if (!IfS) return nullptr;

        // Case 1: the condition itself contains the failing call (allow a
        // logical-not wrapper around it).
        if (const Expr *Cond = IfS->getCond()) {
          const Expr *C = Cond->IgnoreParenImpCasts();
          while (const auto *UO = dyn_cast<UnaryOperator>(C)) {
            if (UO->getOpcode() != UO_LNot)
              break;
            C = UO->getSubExpr()->IgnoreParenImpCasts();
          }
          if (const auto *CE = dyn_cast<CallExpr>(C))
            return CE;
        }

        const CompoundStmt *CS = nullptr;
        unsigned Idx = 0;
        if (!getEnclosingCompoundAndIndex(IfS, Ctx, CS, Idx) || !CS)
          return nullptr;

        // Case 2: bind the guarded value to its producing call. Walk backwards
        // over the enclosing block, skipping unrelated statements, until the
        // statement that produced the condition value from a call (or a direct
        // call statement when the condition is not a plain local read).
        const VarDecl *CondVar = nullptr;
        if (const Expr *Cond = IfS->getCond()) {
          const auto *DRE = dyn_cast<DeclRefExpr>(Cond->IgnoreParenImpCasts());
          if (DRE)
            CondVar = dyn_cast<VarDecl>(DRE->getDecl());
        }

        for (unsigned K = Idx; K > 0; --K) {
          const Stmt *Prev = getStmtAtIndex(CS, K - 1);
          if (!Prev) continue;

          const CallExpr *CE = getCallFromAssignment(Prev);
          if (!CE)
            CE = getCallFromDeclInit(Prev);
          if (!CE && isa<CallExpr>(Prev))
            CE = cast<CallExpr>(Prev);

          if (CE) {
            if (!CondVar || getCallResultVar(Prev) == CondVar)
              return CE;
            continue; // Produces some other value; not the guarded producer.
          }

          // Stop the backward search at control-flow boundaries.
          if (isa<IfStmt>(Prev) || isa<SwitchStmt>(Prev) || isa<ForStmt>(Prev) ||
              isa<WhileStmt>(Prev) || isa<DoStmt>(Prev) || isa<LabelStmt>(Prev) ||
              isa<ReturnStmt>(Prev) || isa<GotoStmt>(Prev))
            break;
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

      // Collect free-like calls under one statement of the cleanup region.
      static void collectFreesUnderLabel(const Stmt *S, StringRef LName,
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

          // The statement the label tags belongs to this cleanup region:
          // in `label:\n  kfree(x);` the first free sits inside the LabelStmt
          // as its substatement, not in a sibling position.
          collectFreesUnderLabel(LS->getSubStmt(), LName, OutMap);

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
            collectFreesUnderLabel(Cur, LName, OutMap);

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

      static bool exprIsDeclRef(const Expr *E, const VarDecl *V) {
        if (!E) return false;
        const auto *DRE = dyn_cast<DeclRefExpr>(E->IgnoreParenImpCasts());
        return DRE && DRE->getDecl() == V;
      }

      static bool exprIsParamField(const Expr *E, const VarDecl *Param,
                                   const FieldDecl *FD) {
        if (!E) return false;
        const auto *ME = dyn_cast<MemberExpr>(E->IgnoreParenImpCasts());
        if (!ME) return false;
        const Expr *Base = ME->getBase();
        const auto *DRE = dyn_cast<DeclRefExpr>(Base ? Base->IgnoreParenImpCasts() : nullptr);
        return DRE && DRE->getDecl() == Param && ME->getMemberDecl() == FD;
      }

      // True when a statement tree aliases Local to Param->FD in either
      // direction: Local = Param->FD (load), Param->FD = Local (publish), or a
      // local declaration of Local initialized from Param->FD.
      static bool stmtAliasesLocalToField(const Stmt *S, const VarDecl *Local,
                                          const VarDecl *Param, const FieldDecl *FD) {
        if (!S) return false;
        if (const auto *BO = dyn_cast<BinaryOperator>(S)) {
          if (BO->isAssignmentOp()) {
            const Expr *LHS = BO->getLHS();
            const Expr *RHS = BO->getRHS();
            if ((exprIsDeclRef(LHS, Local) && exprIsParamField(RHS, Param, FD)) ||
                (exprIsParamField(LHS, Param, FD) && exprIsDeclRef(RHS, Local)))
              return true;
          }
        } else if (const auto *DS = dyn_cast<DeclStmt>(S)) {
          for (const Decl *Di : DS->decls()) {
            const auto *VD = dyn_cast<VarDecl>(Di);
            if (VD == Local && VD->getInit() && exprIsParamField(VD->getInit(), Param, FD))
              return true;
          }
        }
        for (const Stmt *Child : S->children()) {
          if (Child && stmtAliasesLocalToField(Child, Local, Param, FD))
            return true;
        }
        return false;
      }

      // Strip parentheses and implicit/explicit casts so that a free of
      // "(void *)param->field" still binds to the released field itself.
      static const Expr *unwrapParensAndCasts(const Expr *E) {
        for (int Guard = 0; E && Guard < 16; ++Guard) {
          E = E->IgnoreParens();
          const auto *CE = dyn_cast<CastExpr>(E);
          if (!CE)
            return E;
          E = CE->getSubExpr();
        }
        return E;
      }

      // True when the failing callee's own body releases field FD of its
      // parameter Param - directly (free(Param->FD)), through a local alias
      // of the field, or by delegating the same object to a bounded chain of
      // direct callees that release it through their own parameter. On that
      // callee's error path the field has already been freed, so a caller
      // cleanup free of the same field is a double free.
      static bool bodyReleasesParamField(const Stmt *S, const VarDecl *Param,
                                         const FieldDecl *FD, int Depth) {
        if (!S || Depth < 0)
          return false;
        llvm::SmallVector<const CallExpr*, 8> Calls;
        collectAllCallsIn(S, Calls);
        for (const CallExpr *CE : Calls) {
          const FunctionDecl *Callee = CE->getDirectCallee();
          if (!Callee)
            continue;

          if (Callee->getIdentifier() &&
              isKnownFreeName(Callee->getIdentifier()->getName()) &&
              CE->getNumArgs() > 0) {
            const Expr *Arg0 = unwrapParensAndCasts(CE->getArg(0));
            if (exprIsParamField(Arg0, Param, FD))
              return true;
            const auto *DRE = dyn_cast<DeclRefExpr>(Arg0);
            if (DRE) {
              const auto *Local = dyn_cast<VarDecl>(DRE->getDecl());
              if (Local && Local->isLocalVarDecl() &&
                  stmtAliasesLocalToField(S, Local, Param, FD))
                return true;
            }
          }

          // Ownership delegation: when the callee hands Param itself (or the
          // field) to another function, that function's body may release the
          // same field through its corresponding parameter.
          if (Depth == 0 || !Callee->getIdentifier() || CE->getNumArgs() == 0)
            continue;
          const FunctionDecl *CalleeDef = Callee->getDefinition();
          if (!CalleeDef || !CalleeDef->hasBody() || CalleeDef->getBody() == S)
            continue;
          for (unsigned I = 0;
               I < CE->getNumArgs() && I < CalleeDef->getNumParams(); ++I) {
            const Expr *Arg = unwrapParensAndCasts(CE->getArg(I));
            bool PassesObject = false;
            if (const auto *ADRE = dyn_cast<DeclRefExpr>(Arg))
              PassesObject = (ADRE->getDecl() == Param);
            if (exprIsParamField(Arg, Param, FD))
              PassesObject = true;
            if (PassesObject &&
                bodyReleasesParamField(CalleeDef->getBody(),
                                       CalleeDef->getParamDecl(I), FD,
                                       Depth - 1))
              return true;
          }
        }
        return false;
      }

      // Variable written by a "var = call()" assignment or a "T var = call()"
      // declaration; used to bind a guarded value to its producing call.
      static const VarDecl *getCallResultVar(const Stmt *S) {
        if (!S) return nullptr;
        if (const auto *BO = dyn_cast<BinaryOperator>(S)) {
          if (BO->isAssignmentOp()) {
            const Expr *LHS = BO->getLHS();
            const auto *DRE =
                dyn_cast<DeclRefExpr>(LHS ? LHS->IgnoreParenImpCasts() : nullptr);
            if (DRE)
              return dyn_cast<VarDecl>(DRE->getDecl());
          }
        } else if (const auto *DS = dyn_cast<DeclStmt>(S)) {
          for (const Decl *Di : DS->decls()) {
            if (const auto *VD = dyn_cast<VarDecl>(Di))
              return VD;
          }
        }
        return nullptr;
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

      // Double-free mechanism: the early goto jumps to a cleanup region that
      // frees a field of an object also passed to the failing call; when that
      // call's callee releases the same field (directly or through a local
      // alias) on its own error path, the cleanup free is a second free.
      // Require this release relation for the report; a failing callee that
      // leaves the field owned keeps the cleanup free silent.
      {
        const auto *FreedME = dyn_cast<MemberExpr>(E->IgnoreParenImpCasts());
        const auto *FreedField =
            FreedME ? dyn_cast<FieldDecl>(FreedME->getMemberDecl()) : nullptr;
        const FunctionDecl *CalleeFn = FailingCall->getDirectCallee();
        unsigned BaseArgIdx = FailingCall->getNumArgs();
        for (unsigned I = 0; I < FailingCall->getNumArgs(); ++I) {
          const Expr *Arg = FailingCall->getArg(I)->IgnoreParenImpCasts();
          const auto *ADRE = dyn_cast<DeclRefExpr>(Arg);
          if (ADRE && ADRE->getDecl() == BVD) {
            BaseArgIdx = I;
            break;
          }
        }

        bool CalleeReleasesField = false;
        if (CalleeFn && FreedField && BaseArgIdx < FailingCall->getNumArgs()) {
          const FunctionDecl *CalleeDef = CalleeFn->getDefinition();
          if (CalleeDef && CalleeDef->hasBody() &&
              BaseArgIdx < CalleeDef->getNumParams()) {
            const VarDecl *CalleeParam = CalleeDef->getParamDecl(BaseArgIdx);
            CalleeReleasesField =
                bodyReleasesParamField(CalleeDef->getBody(), CalleeParam,
                                       FreedField, 2);
          }
        }

        if (!CalleeReleasesField)
          continue; // Failing callee does not release this field: not a double free
      }

      // Step E: Report
      PathDiagnosticLocation Loc = PathDiagnosticLocation::createBegin(FT.FreeCallCE, BR.getSourceManager(), ADC);
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
