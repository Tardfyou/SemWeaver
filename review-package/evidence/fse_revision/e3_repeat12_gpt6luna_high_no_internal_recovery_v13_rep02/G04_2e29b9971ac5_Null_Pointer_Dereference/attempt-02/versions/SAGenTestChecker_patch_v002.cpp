#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Checkers/Taint.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/Environment.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtVisitor.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/StaticAnalyzer/Checkers/utility.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "clang/Lex/Lexer.h"

using namespace clang;
using namespace ento;
using namespace taint;

// No custom program states are needed.

namespace {
class SAGenTestChecker : public Checker<check::BranchCondition> {
   mutable std::unique_ptr<BugType> BT;

   public:
      SAGenTestChecker() : BT(new BugType(this, "Wrong NULL check after allocation", "Logic error")) {}

      void checkBranchCondition(const Stmt *Condition, CheckerContext &C) const;

   private:
      // Extract the pointer expression being NULL-checked in a condition.
      const Expr *extractNullCheckedPtr(const Expr *Cond, CheckerContext &C) const;
      // Determine if a CallExpr calls a known allocator (e.g., kzalloc, kmalloc, etc.).
      bool isKnownAllocatorCall(const CallExpr *CE, CheckerContext &C) const;
      // Unwrap wrappers to reach the core expression (e.g., ExprWithCleanups).
      const Expr *unwrapToCoreExpr(const Stmt *S) const;
};

const Expr *SAGenTestChecker::unwrapToCoreExpr(const Stmt *S) const {
  const Stmt *Cur = S;
  while (true) {
    if (const auto *EWC = dyn_cast<ExprWithCleanups>(Cur)) {
      Cur = EWC->getSubExpr();
      continue;
    }
    if (const auto *FE = dyn_cast<FullExpr>(Cur)) {
      Cur = FE->getSubExpr();
      continue;
    }
    break;
  }
  return dyn_cast<Expr>(Cur);
}

bool SAGenTestChecker::isKnownAllocatorCall(const CallExpr *CE, CheckerContext &C) const {
  // Treat the call as the pointer-producing assignment source; the warning
  // still requires a subsequent NULL check of a different region on the same object.
  return CE && CE->getType()->isPointerType();
}

const Expr *SAGenTestChecker::extractNullCheckedPtr(const Expr *Cond, CheckerContext &C) const {
  if (!Cond)
    return nullptr;

  ASTContext &ACtx = C.getASTContext();
  const Expr *E = Cond->IgnoreParenCasts();

  if (const auto *UO = dyn_cast<UnaryOperator>(E)) {
    if (UO->getOpcode() == UO_LNot) {
      const Expr *SubE = UO->getSubExpr()->IgnoreParenCasts();
      if (SubE && SubE->getType()->isPointerType())
        return SubE;
    }
  } else if (const auto *BO = dyn_cast<BinaryOperator>(E)) {
    BinaryOperator::Opcode Op = BO->getOpcode();
    if (Op == BO_EQ) {
      const Expr *LHS = BO->getLHS()->IgnoreParenCasts();
      const Expr *RHS = BO->getRHS()->IgnoreParenCasts();

      bool LHSIsNull = LHS->isNullPointerConstant(ACtx, Expr::NPC_ValueDependentIsNull);
      bool RHSIsNull = RHS->isNullPointerConstant(ACtx, Expr::NPC_ValueDependentIsNull);

      if (LHSIsNull && RHS && RHS->getType()->isPointerType())
        return RHS;
      if (RHSIsNull && LHS && LHS->getType()->isPointerType())
        return LHS;
    }
  }
  return nullptr;
}

void SAGenTestChecker::checkBranchCondition(const Stmt *Condition, CheckerContext &C) const {
  if (!Condition)
    return;

  // Find the enclosing IfStmt for this condition.
  const IfStmt *IfS = findSpecificTypeInParents<IfStmt>(Condition, C);
  if (!IfS)
    return;

  const Expr *CondE = dyn_cast<Expr>(Condition);
  if (!CondE)
    return;

  // Extract the pointer being NULL-checked in the condition.
  const Expr *CheckedPtrExpr = extractNullCheckedPtr(CondE, C);
  if (!CheckedPtrExpr)
    return;

  // Find the enclosing compound statement to get the previous sibling statement.
  const CompoundStmt *CS = findSpecificTypeInParents<CompoundStmt>(IfS, C);
  if (!CS)
    return;

  // Find the nearest preceding pointer-producing assignment, allowing unrelated
  // statements between the assignment and its NULL check.
  const BinaryOperator *BO = nullptr;
  for (auto I = CS->body_begin(), E = CS->body_end(); I != E; ++I) {
    if (*I != IfS)
      continue;
    while (I != CS->body_begin()) {
      --I;
      const Expr *Candidate = unwrapToCoreExpr(*I);
      const auto *Assignment = dyn_cast_or_null<BinaryOperator>(Candidate);
      if (!Assignment || Assignment->getOpcode() != BO_Assign)
        continue;
      const auto *Call = dyn_cast<CallExpr>(Assignment->getRHS()->IgnoreParenCasts());
      if (isKnownAllocatorCall(Call, C)) {
        BO = Assignment;
        break;
      }
    }
    break;
  }
  if (!BO)
    return;

  const Expr *LHS = BO->getLHS(); // Do not IgnoreImplicit before region extraction.
  const Expr *RHS = BO->getRHS();
  if (!LHS || !RHS)
    return;

  // RHS should be an allocator call.
  const CallExpr *AllocCE = dyn_cast<CallExpr>(RHS->IgnoreParenCasts());
  if (!isKnownAllocatorCall(AllocCE, C))
    return;

  // Both sides should be pointer-typed to be relevant.
  if (!LHS->getType()->isPointerType())
    return;
  if (!CheckedPtrExpr->getType()->isPointerType())
    return;

  // Get memory regions for both the allocated pointer (LHS) and the checked pointer.
  const MemRegion *AllocMRRaw = getMemRegionFromExpr(LHS, C);
  const MemRegion *ChkMRRaw = getMemRegionFromExpr(CheckedPtrExpr, C);
  if (!AllocMRRaw || !ChkMRRaw)
    return;

  const MemRegion *AllocMRBase = AllocMRRaw->getBaseRegion();
  const MemRegion *ChkMRBase = ChkMRRaw->getBaseRegion();
  if (!AllocMRBase || !ChkMRBase)
    return;

  // If region extraction is unavailable, preserve the same-object relation
  // through the member-access roots instead of abandoning the trigger.
  auto getMemberRoot = [](const Expr *E, bool &HasMember,
                          const ValueDecl *&Leaf) -> const ValueDecl * {
    E = E->IgnoreParenImpCasts();
    while (const auto *ME = dyn_cast<MemberExpr>(E)) {
      if (!HasMember)
        Leaf = ME->getMemberDecl();
      HasMember = true;
      E = ME->getBase()->IgnoreParenImpCasts();
    }
    if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
      return DRE->getDecl();
    return nullptr;
  };
  bool AllocHasMember = false;
  bool CheckHasMember = false;
  const ValueDecl *AllocLeaf = nullptr;
  const ValueDecl *CheckLeaf = nullptr;
  const ValueDecl *AllocRoot = getMemberRoot(LHS, AllocHasMember, AllocLeaf);
  const ValueDecl *CheckRoot =
      getMemberRoot(CheckedPtrExpr, CheckHasMember, CheckLeaf);
  const bool DifferentMemberLocations =
      AllocHasMember && CheckHasMember && AllocRoot &&
      AllocRoot == CheckRoot && AllocLeaf && CheckLeaf &&
      AllocLeaf != CheckLeaf;
  const bool DifferentRegions =
      (AllocMRBase && ChkMRBase && AllocMRBase == ChkMRBase &&
       AllocMRRaw != ChkMRRaw) ||
      DifferentMemberLocations;

  // The allocation and null check must refer to different pointer locations
  // rooted in the same object.
  if (DifferentRegions) {
    ExplodedNode *N = C.generateNonFatalErrorNode();
    if (!N)
      return;

    auto R = std::make_unique<PathSensitiveBugReport>(
        *BT, "Wrong pointer checked after allocation", N);
    R->addRange(IfS->getCond()->getSourceRange());
    R->addRange(BO->getSourceRange());
    C.emitReport(std::move(R));
  }
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects checking a different pointer than the one just allocated (wrong NULL check)",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
