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
#include "llvm/ADT/FoldingSet.h"
#include "clang/StaticAnalyzer/Checkers/utility.h"
#include "clang/Lex/Lexer.h"

using namespace clang;
using namespace ento;
using namespace taint;

namespace {
/* The checker callbacks are to be decided. */
class SAGenTestChecker : public Checker<check::PreCall> {
   mutable std::unique_ptr<BugType> BT;

   public:
      SAGenTestChecker() : BT(new BugType(this, "Overflow-prone allocation size (use kcalloc)", "API Misuse")) {}

      void checkPreCall(const CallEvent &Call, CheckerContext &C) const;

   private:

      // Return true if Call is one of the array-aware allocators that should be ignored.
      bool isArrayAwareAllocator(const CallEvent &Call, CheckerContext &C) const;

      // If Call is a target allocator that takes a single total size parameter,
      // set Idx to the index of that size argument and return true.
      bool getAllocatorSizeArgIndex(const CallEvent &Call, unsigned &Idx, CheckerContext &C) const;

      // Returns true if expression subtree contains a sizeof(...) (UnaryExprOrTypeTraitExpr of kind SizeOf).
      static bool exprContainsSizeof(const Expr *E);

      // Report helper
      void reportMulPattern(const BinaryOperator *Mul, CheckerContext &C) const;

      // Returns true when the count operand is already bounded by kernel-side
      // bookkeeping in the enclosing function: it is an internal parameter, a
      // local filled through an address-taken output parameter, or an
      // expression already tied to another value by an equality comparison.
      bool isKernelBoundedCount(const Expr *CountE, CheckerContext &C) const;

      // Body statement of the function containing the analyzed call.
      static const Stmt *getEnclosingBody(CheckerContext &C);
};

// Finds whether the address of a local variable is taken in a statement
// subtree, i.e., the variable is filled through an output parameter.
class AddrTakenFinder : public RecursiveASTVisitor<AddrTakenFinder> {
public:
  explicit AddrTakenFinder(const VarDecl *V) : Var(V) {}

  bool VisitUnaryOperator(UnaryOperator *UO) {
    if (Found)
      return false;
    if (UO->getOpcode() == UO_AddrOf) {
      const Expr *Sub = UO->getSubExpr()->IgnoreParenImpCasts();
      const auto *DRE = dyn_cast<DeclRefExpr>(Sub);
      if (DRE && DRE->getDecl() == Var)
        Found = true;
    }
    return !Found;
  }

  bool Found = false;

private:
  const VarDecl *Var;
};

// Finds whether an expression appears (canonically) as an operand of an
// equality or inequality comparison in a statement subtree.
class EqualityCompareFinder : public RecursiveASTVisitor<EqualityCompareFinder> {
public:
  explicit EqualityCompareFinder(const Expr *E, ASTContext &Ctx)
      : Count(E), Ctx(Ctx) {}

  bool VisitBinaryOperator(BinaryOperator *BO) {
    if (Found)
      return false;
    if (BO->getOpcode() == BO_EQ || BO->getOpcode() == BO_NE) {
      if (sameExpr(BO->getLHS()) || sameExpr(BO->getRHS()))
        Found = true;
    }
    return !Found;
  }

  bool Found = false;

private:
  bool sameExpr(const Expr *E) const {
    if (!E || !Count)
      return false;
    llvm::FoldingSetNodeID A, B;
    E->IgnoreParenImpCasts()->Profile(A, Ctx, true);
    Count->IgnoreParenImpCasts()->Profile(B, Ctx, true);
    return A == B;
  }

  const Expr *Count;
  ASTContext &Ctx;
};

bool SAGenTestChecker::isArrayAwareAllocator(const CallEvent &Call, CheckerContext &C) const {
  const Expr *Orig = Call.getOriginExpr();
  if (!Orig)
    return false;

  // Ignore calls that already use overflow-safe array helpers.
  static const char *ArrayAware[] = {
      "kcalloc",
      "kvcalloc",
      "kmalloc_array",
      "kvmalloc_array",
      "devm_kcalloc"
  };

  for (const char *Name : ArrayAware) {
    if (ExprHasName(Orig, Name, C))
      return true;
  }
  return false;
}

bool SAGenTestChecker::getAllocatorSizeArgIndex(const CallEvent &Call, unsigned &Idx, CheckerContext &C) const {
  const Expr *Orig = Call.getOriginExpr();
  if (!Orig)
    return false;

  // Order matters where names can be substrings of others. Keep more specific first.
  struct Entry { const char *Name; unsigned SizeIdx; };
  static const Entry Targets[] = {
      {"devm_kzalloc", 1},
      {"devm_kmalloc", 1},
      {"kvzalloc", 0},
      {"kvmalloc", 0},
      {"kzalloc", 0},
      {"kmalloc", 0},
      {"vzalloc", 0},
  };

  for (const auto &E : Targets) {
    if (ExprHasName(Orig, E.Name, C)) {
      Idx = E.SizeIdx;
      return true;
    }
  }
  return false;
}

bool SAGenTestChecker::exprContainsSizeof(const Expr *E) {
  if (!E) return false;
  const Stmt *S = dyn_cast<Stmt>(E);
  if (!S) return false;

  const auto *UETT = findSpecificTypeInChildren<UnaryExprOrTypeTraitExpr>(S);
  if (!UETT) return false;
  return UETT->getKind() == UETT_SizeOf;
}

const Stmt *SAGenTestChecker::getEnclosingBody(CheckerContext &C) {
  const LocationContext *LCtx = C.getLocationContext();
  if (!LCtx)
    return nullptr;
  const Decl *D = LCtx->getDecl();
  if (!D)
    return nullptr;
  const auto *FD = dyn_cast<FunctionDecl>(D);
  if (!FD)
    return nullptr;
  return FD->getBody();
}

bool SAGenTestChecker::isKernelBoundedCount(const Expr *CountE, CheckerContext &C) const {
  if (!CountE)
    return false;

  if (const auto *DRE = dyn_cast<DeclRefExpr>(CountE)) {
    const auto *VD = dyn_cast<VarDecl>(DRE->getDecl());

    // The count reaches the sink as an internal function parameter: it is
    // plumbed by the in-kernel call graph rather than read from a request
    // object at the allocation.
    if (VD && dyn_cast<ParmVarDecl>(VD))
      return true;

    // The count is a local variable filled through an address-taken output
    // parameter of a call in the same function: a kernel-measured quantity.
    if (VD && VD->isLocalVarDecl()) {
      const Stmt *Body = getEnclosingBody(C);
      if (Body) {
        AddrTakenFinder Finder(VD);
        Finder.TraverseStmt(const_cast<Stmt *>(Body));
        if (Finder.Found)
          return true;
      }
    }
  }

  // The count is already tied to another value by an equality/inequality
  // comparison in the same function: the request count is validated against
  // kernel-measured state before the allocation.
  const Stmt *Body = getEnclosingBody(C);
  if (Body) {
    EqualityCompareFinder Finder(CountE, C.getASTContext());
    Finder.TraverseStmt(const_cast<Stmt *>(Body));
    if (Finder.Found)
      return true;
  }

  return false;
}

void SAGenTestChecker::reportMulPattern(const BinaryOperator *Mul, CheckerContext &C) const {
  if (!Mul) return;
  ExplodedNode *N = C.generateNonFatalErrorNode();
  if (!N) return;

  auto R = std::make_unique<PathSensitiveBugReport>(
      *BT,
      "Use kcalloc(count, size, ...) instead of count*sizeof in allocation to avoid integer overflow",
      N);
  R->addRange(Mul->getSourceRange());
  C.emitReport(std::move(R));
}

void SAGenTestChecker::checkPreCall(const CallEvent &Call, CheckerContext &C) const {
  // Ignore non-interesting calls first.
  if (isArrayAwareAllocator(Call, C))
    return;

  unsigned SizeIdx = 0;
  if (!getAllocatorSizeArgIndex(Call, SizeIdx, C))
    return;

  if (Call.getNumArgs() <= SizeIdx)
    return;

  const Expr *SizeE = Call.getArgExpr(SizeIdx);
  if (!SizeE)
    return;

  // Suppress safe helpers used inside size expression.
  if (ExprHasName(SizeE, "array_size", C) ||
      ExprHasName(SizeE, "struct_size", C) ||
      ExprHasName(SizeE, "flex_array_size", C))
    return;

  SizeE = SizeE->IgnoreParenImpCasts();
  const auto *BO = dyn_cast<BinaryOperator>(SizeE);
  if (!BO || BO->getOpcode() != BO_Mul)
    return;

  const Expr *LHS = BO->getLHS()->IgnoreParenImpCasts();
  const Expr *RHS = BO->getRHS()->IgnoreParenImpCasts();

  // Look for sizeof(...) on either side to match "count * sizeof(T)" or "sizeof(T) * count"
  bool HasSizeofLHS = exprContainsSizeof(LHS);
  bool HasSizeofRHS = exprContainsSizeof(RHS);
  if (!HasSizeofLHS && !HasSizeofRHS)
    return;

  // The count operand is the side that does not contain sizeof(...).
  const Expr *CountE = (HasSizeofLHS && !HasSizeofRHS) ? RHS : LHS;

  // The overflow-prone pattern is the request-borne count multiplied at the
  // sink; a count the enclosing function already derives from kernel-side
  // state or validates by equality is out of scope for this relation.
  if (isKernelBoundedCount(CountE, C))
    return;

  reportMulPattern(BO, C);
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detects kmalloc/kzalloc-style allocations that multiply count by sizeof; suggest kcalloc to avoid overflow",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
