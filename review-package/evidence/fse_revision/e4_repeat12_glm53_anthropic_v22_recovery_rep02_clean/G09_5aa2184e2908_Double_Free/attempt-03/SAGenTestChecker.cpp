#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/Analysis/AnalysisDeclContext.h"
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

      // Extract the status variable tested by an early-exit guard: `if (v)` or
      // `if (!v)`. Returns null for any other condition shape.
      static const VarDecl *getStatusVarFromCond(const Expr *Cond) {
        if (!Cond) return nullptr;
        const Expr *E = Cond->IgnoreParenImpCasts();
        if (const auto *UO = dyn_cast<UnaryOperator>(E)) {
          if (UO->getOpcode() == UO_LNot)
            E = UO->getSubExpr()->IgnoreParenImpCasts();
        }
        if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
          return dyn_cast<VarDecl>(DRE->getDecl());
        return nullptr;
      }

      // Match a top-level statement of the form `Status = <call>(...)` so the
      // failing producer is identified by a value relation, not adjacency.
      static const CallExpr *getCallAssignedToVar(const Stmt *S, const VarDecl *Status) {
        if (!S || !Status) return nullptr;
        const auto *BO = dyn_cast<BinaryOperator>(S);
        if (!BO || !BO->isAssignmentOp()) return nullptr;
        const Expr *LHS = BO->getLHS();
        if (!LHS) return nullptr;
        const auto *DRE = dyn_cast<DeclRefExpr>(LHS->IgnoreParenImpCasts());
        if (!DRE || DRE->getDecl() != Status) return nullptr;
        const Expr *RHS = BO->getRHS();
        return dyn_cast_or_null<CallExpr>(RHS ? RHS->IgnoreParenImpCasts() : nullptr);
      }

      static const CallExpr *findCallBeforeIf(const IfStmt *IfS, ASTContext &Ctx) {
        if (!IfS) return nullptr;

        // Preferred: bind the producer to the guard by a value relation. The
        // goto guard tests a status variable; scan preceding siblings for the
        // nearest `status = producer(...)` assignment of that same variable,
        // so unrelated statements between producer and guard keep the link.
        if (const VarDecl *StatusVar = getStatusVarFromCond(IfS->getCond())) {
          const CompoundStmt *CS = nullptr;
          unsigned Idx = 0;
          if (getEnclosingCompoundAndIndex(IfS, Ctx, CS, Idx) && CS) {
            for (unsigned I = Idx; I-- > 0;) {
              const Stmt *Cur = getStmtAtIndex(CS, I);
              if (!Cur) continue;
              if (const CallExpr *CE = getCallAssignedToVar(Cur, StatusVar))
                return CE;
            }
          }
        }

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

        const Stmt *Prev = getStmtAtIndex(CS, Idx - 1);
        if (!Prev) return nullptr;

        if (const CallExpr *CE = getCallFromAssignment(Prev))
          return CE;
        if (const CallExpr *CE = getCallFromDeclInit(Prev))
          return CE;

        // Also consider if the previous statement is directly a call.
        if (const auto *CE = dyn_cast<CallExpr>(Prev))
          return CE;

        // Or contains a call anywhere (less strict)
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

          // Scan the cleanup region starting from the label's own
          // sub-statement (the statement it prefixes) and continuing with the
          // following siblings.
          unsigned CSSize = getCompoundBodySize(CS);
          for (unsigned I = Idx; I < CSSize; ++I) {
            const Stmt *Cur = (I == Idx) ? LS->getSubStmt() : getStmtAtIndex(CS, I);
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

      // True when the producer call reclaims the member buffer `Arg->Field`
      // before returning a failing status. Reclamation is proven inside the
      // producer's visible call subtree: a direct free of the bound
      // parameter's member, a free of a local aliasing that member buffer,
      // a helper receiving the bound parameter that reclaims it in turn, or
      // a helper receiving the member buffer itself and freeing it.
      static bool producerReclaimsMember(const CallExpr *CE, const VarDecl *Arg,
                                         StringRef Field) {
        if (!CE || !Arg) return false;
        const FunctionDecl *CalleeFD = CE->getDirectCallee();
        if (!CalleeFD) return false;
        const FunctionDecl *Def = CalleeFD->getDefinition();
        if (!Def || !Def->hasBody()) return false;
        const Stmt *CalleeBody = Def->getBody();
        if (!CalleeBody) return false;

        // Bind the caller's argument to the callee parameter it feeds.
        const ParmVarDecl *BoundParam = bindArgToParam(CE, Def, Arg);
        if (!BoundParam) return false;

        return bodyReclaimsMember(CalleeBody, BoundParam, Field, 2);
      }

      // Bind the caller's argument variable to the parameter position it
      // occupies in this call.
      static const ParmVarDecl *bindArgToParam(const CallExpr *CE,
                                               const FunctionDecl *Def,
                                               const VarDecl *Arg) {
        for (unsigned i = 0; i < CE->getNumArgs() && i < Def->getNumParams(); ++i) {
          const auto *DRE = dyn_cast<DeclRefExpr>(CE->getArg(i)->IgnoreParenImpCasts());
          if (!DRE) continue;
          const auto *VD = dyn_cast<VarDecl>(DRE->getDecl());
          if (VD == Arg)
            return Def->getParamDecl(i);
        }
        return nullptr;
      }

      static const VarDecl *getVarDeclFromExpr(const Expr *E) {
        if (!E) return nullptr;
        const auto *DRE = dyn_cast<DeclRefExpr>(E->IgnoreParenImpCasts());
        if (!DRE) return nullptr;
        return dyn_cast<VarDecl>(DRE->getDecl());
      }

      // Collect the locals of this body that hold the `Param->Field` buffer,
      // linked by an assignment in either direction or a declaration
      // initializer (value relation, independent of spelling or adjacency).
      static void collectMemberAliasLocals(const Stmt *S, const ParmVarDecl *Param,
                                           StringRef Field,
                                           llvm::SmallVectorImpl<const VarDecl*> &Out) {
        if (!S) return;
        if (const auto *BO = dyn_cast<BinaryOperator>(S)) {
          if (BO->isAssignmentOp()) {
            std::string FieldName;
            if (isMemberOfVar(BO->getRHS(), Param, FieldName) && Field.equals(FieldName)) {
              if (const VarDecl *L = getVarDeclFromExpr(BO->getLHS()))
                Out.push_back(L); // L = Param->Field
            }
            if (isMemberOfVar(BO->getLHS(), Param, FieldName) && Field.equals(FieldName)) {
              if (const VarDecl *L = getVarDeclFromExpr(BO->getRHS()))
                Out.push_back(L); // Param->Field = L
            }
          }
        }
        if (const auto *DS = dyn_cast<DeclStmt>(S)) {
          for (const Decl *Di : DS->decls()) {
            if (const auto *VD = dyn_cast<VarDecl>(Di)) {
              std::string FieldName;
              if (VD->getInit() && isMemberOfVar(VD->getInit(), Param, FieldName) &&
                  Field.equals(FieldName))
                Out.push_back(VD); // type L = Param->Field
            }
          }
        }
        for (const Stmt *Child : S->children()) {
          if (Child) collectMemberAliasLocals(Child, Param, Field, Out);
        }
      }

      // Collect locals assigned from, or initialized with, parameter P.
      static void collectParamAliasLocals(const Stmt *S, const ParmVarDecl *P,
                                          llvm::SmallVectorImpl<const VarDecl*> &Out) {
        if (!S) return;
        if (const auto *BO = dyn_cast<BinaryOperator>(S)) {
          if (BO->isAssignmentOp() && getVarDeclFromExpr(BO->getRHS()) == P) {
            if (const VarDecl *L = getVarDeclFromExpr(BO->getLHS()))
              Out.push_back(L);
          }
        }
        if (const auto *DS = dyn_cast<DeclStmt>(S)) {
          for (const Decl *Di : DS->decls()) {
            if (const auto *VD = dyn_cast<VarDecl>(Di)) {
              if (getVarDeclFromExpr(VD->getInit()) == P)
                Out.push_back(VD);
            }
          }
        }
        for (const Stmt *Child : S->children()) {
          if (Child) collectParamAliasLocals(Child, P, Out);
        }
      }

      // True when this body frees the pointer value received in parameter
      // ParamIdx, either that parameter itself or a local assigned from it.
      static bool bodyFreesParamValue(const FunctionDecl *Def, unsigned ParamIdx) {
        if (!Def || !Def->hasBody()) return false;
        const Stmt *Body = Def->getBody();
        if (!Body || ParamIdx >= Def->getNumParams()) return false;
        const ParmVarDecl *P = Def->getParamDecl(ParamIdx);

        llvm::SmallVector<const VarDecl*, 8> Aliases;
        Aliases.push_back(P);
        collectParamAliasLocals(Body, P, Aliases);

        llvm::SmallVector<const CallExpr*, 32> Calls;
        collectAllCallsIn(Body, Calls);
        for (const CallExpr *FC : Calls) {
          const FunctionDecl *FFD = FC->getDirectCallee();
          if (!FFD || !FFD->getIdentifier()) continue;
          if (!isKnownFreeName(FFD->getIdentifier()->getName())) continue;
          if (llvm::is_contained(Aliases, getVarDeclFromExpr(getFreedExprFromCall(FC))))
            return true;
        }
        return false;
      }

      // Does the producer subtree rooted at this body reclaim `Param->Field`?
      // Depth bounds helper delegation so recursive producers terminate.
      static bool bodyReclaimsMember(const Stmt *Body, const ParmVarDecl *Param,
                                     StringRef Field, unsigned Depth) {
        if (!Body || !Param) return false;

        llvm::SmallVector<const VarDecl*, 8> MemberAliases;
        collectMemberAliasLocals(Body, Param, Field, MemberAliases);

        llvm::SmallVector<const CallExpr*, 32> Calls;
        collectAllCallsIn(Body, Calls);
        for (const CallExpr *FC : Calls) {
          const FunctionDecl *FFD = FC->getDirectCallee();
          if (!FFD || !FFD->getIdentifier()) continue;
          StringRef FName = FFD->getIdentifier()->getName();

          if (isKnownFreeName(FName)) {
            // Direct free of the member through the bound parameter.
            const Expr *FreedArg = getFreedExprFromCall(FC);
            std::string FreedField;
            if (FreedArg && isMemberOfVar(FreedArg, Param, FreedField) &&
                Field.equals(FreedField))
              return true;
            // Free of a local that holds the member buffer.
            if (llvm::is_contained(MemberAliases, getVarDeclFromExpr(FreedArg)))
              return true;
            continue;
          }

          if (Depth == 0) continue;

          // A helper receiving the bound parameter, or the member buffer
          // itself, may perform the reclamation one level deeper.
          const FunctionDecl *HDef = FFD->getDefinition();
          if (!HDef || !HDef->hasBody()) continue;
          for (unsigned i = 0; i < FC->getNumArgs() && i < HDef->getNumParams(); ++i) {
            const Expr *ArgE = FC->getArg(i)->IgnoreParenImpCasts();
            if (!ArgE) continue;
            if (getVarDeclFromExpr(ArgE) == Param &&
                bodyReclaimsMember(HDef->getBody(), HDef->getParamDecl(i),
                                   Field, Depth - 1))
              return true;
            std::string FwdField;
            if (isMemberOfVar(ArgE, Param, FwdField) && Field.equals(FwdField) &&
                bodyFreesParamValue(HDef, i))
              return true;
          }
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

      // Ownership collision: the cleanup free is a double free only when the
      // failing producer already reclaimed the same member of the struct
      // handed to it (its subtree freeing it directly, through a local alias
      // of the member buffer, or through a helper that receives the bound
      // parameter or the member buffer itself) before
      // returning the failing status. Without that proof the cleanup free is
      // an ordinary single free of a member this path owns, whether this
      // function populated it locally or an earlier successful producer did.
      // The patch fixes the collision by rerouting the goto to a cleanup
      // label whose region skips this kfree.
      if (!producerReclaimsMember(FailingCall, BVD, FieldName))
        continue; // Producer did not reclaim it; ordinary single cleanup free.

      // Step E: Report
      PathDiagnosticLocation Loc = PathDiagnosticLocation::createBegin(FT.FreeCallCE, BR.getSourceManager(), ADC);
      auto R = std::make_unique<BasicBugReport>(
          *BT,
          "Early error path frees a struct member of an argument passed to the failing call although this function never took local ownership of that member; the callee-side cleanup already owns it, risking double free",
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
      "Detects goto-based early error cleanup that frees a struct member handed to the failing producer before this function took local ownership of it",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
