# Role

You are an expert in developing and analyzing Clang Static Analyzer checkers, with decades of experience in the Clang project, particularly in the Static Analyzer plugin.

# Instruction

Please analyze this false positive case and propose fixes to the checker code to eliminate this specific false positive while maintaining detection of true positives.

Please help improve this checker to eliminate the false positive while maintaining its ability to detect actual issues. Your solution should:

1. Identify the root cause of the false positive
2. Propose specific fixes to the checker logic
3. Consider edge cases and possible regressions
4. Maintain compatibility with Clang-18 API

Note, the repaired checker needs to still **detect the target buggy code**.

## Suggestions

1. Use proper visitor patterns and state tracking
2. Handle corner cases gracefully
3. You could register a program state like `REGISTER_MAP_WITH_PROGRAMSTATE(...)` to track the information you need.
4. Follow Clang Static Analyzer best practices for checker development
5. DO NOT remove any existing `#include` in the checker code.

You could add some functions like `bool isFalsePositive(...)` to help you define and detect the false positive.

# Utility Functions

```cpp
// Going upward in an AST tree, and find the Stmt of a specific type
template <typename T>
const T* findSpecificTypeInParents(const Stmt *S, CheckerContext &C);

// Going downward in an AST tree, and find the Stmt of a secific type
// Only return one of the statements if there are many
template <typename T>
const T* findSpecificTypeInChildren(const Stmt *S);

bool EvaluateExprToInt(llvm::APSInt &EvalRes, const Expr *expr, CheckerContext &C) {
  Expr::EvalResult ExprRes;
  if (expr->EvaluateAsInt(ExprRes, C.getASTContext())) {
    EvalRes = ExprRes.Val.getInt();
    return true;
  }
  return false;
}

const llvm::APSInt *inferSymbolMaxVal(SymbolRef Sym, CheckerContext &C) {
  ProgramStateRef State = C.getState();
  const llvm::APSInt *maxVal = State->getConstraintManager().getSymMaxVal(State, Sym);
  return maxVal;
}

// The expression should be the DeclRefExpr of the array
bool getArraySizeFromExpr(llvm::APInt &ArraySize, const Expr *E) {
  if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(E->IgnoreImplicit())) {
    if (const VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
      QualType QT = VD->getType();
      if (const ConstantArrayType *ArrayType = dyn_cast<ConstantArrayType>(QT.getTypePtr())) {
        ArraySize = ArrayType->getSize();
        return true;
      }
    }
  }
  return false;
}

bool getStringSize(llvm::APInt &StringSize, const Expr *E) {
  if (const auto *SL = dyn_cast<StringLiteral>(E->IgnoreImpCasts())) {
    StringSize = llvm::APInt(32, SL->getLength());
    return true;
  }
  return false;
}

const MemRegion* getMemRegionFromExpr(const Expr* E, CheckerContext &C) {
  ProgramStateRef State = C.getState();
  return State->getSVal(E, C.getLocationContext()).getAsRegion();
}

struct KnownDerefFunction {
  const char *Name;                    ///< The function name.
  llvm::SmallVector<unsigned, 4> Params; ///< The parameter indices that get dereferenced.
};

/// \brief Determines if the given call is to a function known to dereference
///        certain pointer parameters.
///
/// This function looks up the call's callee name in a known table of functions
/// that definitely dereference one or more of their pointer parameters. If the
/// function is found, it appends the 0-based parameter indices that are dereferenced
/// into \p DerefParams and returns \c true. Otherwise, it returns \c false.
///
/// \param[in] Call        The function call to examine.
/// \param[out] DerefParams
///     A list of parameter indices that the function is known to dereference.
///
/// \return \c true if the function is found in the known-dereference table,
///         \c false otherwise.
bool functionKnownToDeref(const CallEvent &Call,
                                 llvm::SmallVectorImpl<unsigned> &DerefParams) {
  if (const IdentifierInfo *ID = Call.getCalleeIdentifier()) {
    StringRef FnName = ID->getName();

    for (const auto &Entry : DerefTable) {
      if (FnName.equals(Entry.Name)) {
        // We found the function in our table, copy its param indices
        DerefParams.append(Entry.Params.begin(), Entry.Params.end());
        return true;
      }
    }
  }
  return false;
}

/// \brief Determines if the source text of an expression contains a specified name.
bool ExprHasName(const Expr *E, StringRef Name, CheckerContext &C) {
  if (!E)
    return false;

  // Use const reference since getSourceManager() returns a const SourceManager.
  const SourceManager &SM = C.getSourceManager();
  const LangOptions &LangOpts = C.getLangOpts();
  // Retrieve the source text corresponding to the expression.
  CharSourceRange Range = CharSourceRange::getTokenRange(E->getSourceRange());
  StringRef ExprText = Lexer::getSourceText(Range, SM, LangOpts);

  // Check if the extracted text contains the specified name.
  return ExprText.contains(Name);
}
```

# Clang Check Functions

```cpp
void checkPreStmt (const ReturnStmt *DS, CheckerContext &C) const
 // Pre-visit the Statement.

void checkPostStmt (const DeclStmt *DS, CheckerContext &C) const
 // Post-visit the Statement.

void checkPreCall (const CallEvent &Call, CheckerContext &C) const
 // Pre-visit an abstract "call" event.

void checkPostCall (const CallEvent &Call, CheckerContext &C) const
 // Post-visit an abstract "call" event.

void checkBranchCondition (const Stmt *Condition, CheckerContext &Ctx) const
 // Pre-visit of the condition statement of a branch (such as IfStmt).


void checkLocation (SVal Loc, bool IsLoad, const Stmt *S, CheckerContext &) const
 // Called on a load from and a store to a location.

void checkBind (SVal Loc, SVal Val, const Stmt *S, CheckerContext &) const
 // Called on binding of a value to a location.


void checkBeginFunction (CheckerContext &Ctx) const
 // Called when the analyzer core starts analyzing a function, regardless of whether it is analyzed at the top level or is inlined.

void checkEndFunction (const ReturnStmt *RS, CheckerContext &Ctx) const
 // Called when the analyzer core reaches the end of a function being analyzed regardless of whether it is analyzed at the top level or is inlined.

void checkEndAnalysis (ExplodedGraph &G, BugReporter &BR, ExprEngine &Eng) const
 // Called after all the paths in the ExplodedGraph reach end of path.


bool evalCall (const CallEvent &Call, CheckerContext &C) const
 // Evaluates function call.

ProgramStateRef evalAssume (ProgramStateRef State, SVal Cond, bool Assumption) const
 // Handles assumptions on symbolic values.

ProgramStateRef checkRegionChanges (ProgramStateRef State, const InvalidatedSymbols *Invalidated, ArrayRef< const MemRegion * > ExplicitRegions, ArrayRef< const MemRegion * > Regions, const LocationContext *LCtx, const CallEvent *Call) const
 // Called when the contents of one or more regions change.

void checkASTDecl (const FunctionDecl *D, AnalysisManager &Mgr, BugReporter &BR) const
 // Check every declaration in the AST.

void checkASTCodeBody (const Decl *D, AnalysisManager &Mgr, BugReporter &BR) const
 // Check every declaration that has a statement body in the AST.
```


The following pattern is the checker designed to detect:

## Bug Pattern

Iterating over two parallel arrays with the same index while using the length/limit of the larger array as the loop bound, and then indexing into the smaller array without an additional bound check. Concretely:

for (i = 0; i < SIZE_A; i++) {   // SIZE_A > SIZE_B
    if (A[i] == key)
        return B[i];             // out-of-bounds when i >= SIZE_B
}

Here, A has SIZE_A elements and B has SIZE_B elements; the loop uses SIZE_A but also accesses B[i], causing a buffer overflow when i reaches SIZE_B..


The patch that needs to be detected:

## Patch Description

drm/amd/display: Fix possible buffer overflow in 'find_dcfclk_for_voltage()'

when 'find_dcfclk_for_voltage()' function is looping over
VG_NUM_SOC_VOLTAGE_LEVELS (which is 8), but the size of the DcfClocks
array is VG_NUM_DCFCLK_DPM_LEVELS (which is 7).

When the loop variable i reaches 7, the function tries to access
clock_table->DcfClocks[7]. However, since the size of the DcfClocks
array is 7, the valid indices are 0 to 6. Index 7 is beyond the size of
the array, leading to a buffer overflow.

Reported by smatch & thus fixing the below:
drivers/gpu/drm/amd/amdgpu/../display/dc/clk_mgr/dcn301/vg_clk_mgr.c:550 find_dcfclk_for_voltage() error: buffer overflow 'clock_table->DcfClocks' 7 <= 7

Fixes: 3a83e4e64bb1 ("drm/amd/display: Add dcn3.01 support to DC (v2)")
Cc: Roman Li <Roman.Li@amd.com>
Cc: Rodrigo Siqueira <Rodrigo.Siqueira@amd.com>
Cc: Aurabindo Pillai <aurabindo.pillai@amd.com>
Signed-off-by: Srinivasan Shanmugam <srinivasan.shanmugam@amd.com>
Reviewed-by: Roman Li <roman.li@amd.com>
Signed-off-by: Alex Deucher <alexander.deucher@amd.com>

## Buggy Code

```c
// Function: find_dcfclk_for_voltage in drivers/gpu/drm/amd/display/dc/clk_mgr/dcn301/vg_clk_mgr.c
static unsigned int find_dcfclk_for_voltage(const struct vg_dpm_clocks *clock_table,
		unsigned int voltage)
{
	int i;

	for (i = 0; i < VG_NUM_SOC_VOLTAGE_LEVELS; i++) {
		if (clock_table->SocVoltage[i] == voltage)
			return clock_table->DcfClocks[i];
	}

	ASSERT(0);
	return 0;
}
```

## Bug Fix Patch

```diff
diff --git a/drivers/gpu/drm/amd/display/dc/clk_mgr/dcn301/vg_clk_mgr.c b/drivers/gpu/drm/amd/display/dc/clk_mgr/dcn301/vg_clk_mgr.c
index a5489fe6875f..aa9fd1dc550a 100644
--- a/drivers/gpu/drm/amd/display/dc/clk_mgr/dcn301/vg_clk_mgr.c
+++ b/drivers/gpu/drm/amd/display/dc/clk_mgr/dcn301/vg_clk_mgr.c
@@ -546,6 +546,8 @@ static unsigned int find_dcfclk_for_voltage(const struct vg_dpm_clocks *clock_ta
 	int i;

 	for (i = 0; i < VG_NUM_SOC_VOLTAGE_LEVELS; i++) {
+		if (i >= VG_NUM_DCFCLK_DPM_LEVELS)
+			break;
 		if (clock_table->SocVoltage[i] == voltage)
 			return clock_table->DcfClocks[i];
 	}
```


# False Positive Report

BuildSource:| drivers/gpu/drm/amd/display/dc/clk_mgr/dcn301/vg_clk_mgr.c
### Report Summary

File:|
/work/SemWeaver/artifacts/external/linux/drivers/gpu/drm/amd/amdgpu/../display/dc/clk_mgr/dcn301/vg_clk_mgr.c  
---|---  
Warning:| line 552, column 11  
Loop bound 8 exceeds array 'DcfClocks' size 7; DcfClocks[i] may be out of
bounds  
  
### Annotated Source Code


502   | 			{
503   | 				.voltage = 0,
504   | 				.dcfclk_mhz = 483,
505   | 				.fclk_mhz = 800,
506   | 				.memclk_mhz = 1600,
507   | 				.socclk_mhz = 0,
508   | 			},
509   | 			{
510   | 				.voltage = 0,
511   | 				.dcfclk_mhz = 602,
512   | 				.fclk_mhz = 1067,
513   | 				.memclk_mhz = 1067,
514   | 				.socclk_mhz = 0,
515   | 			},
516   | 			{
517   | 				.voltage = 0,
518   | 				.dcfclk_mhz = 738,
519   | 				.fclk_mhz = 1333,
520   | 				.memclk_mhz = 1600,
521   | 				.socclk_mhz = 0,
522   | 			},
523   | 		},
524   |  
525   | 		.num_entries = 4,
526   | 	},
527   |  
528   | };
529   |  
530   | static uint32_t find_max_clk_value(const uint32_t clocks[], uint32_t num_clocks)
531   | {
532   | 	uint32_t max = 0;
533   |  int i;
534   |  
535   |  for (i = 0; i < num_clocks; ++i) {
536   |  if (clocks[i] > max)
537   | 			max = clocks[i];
538   | 	}
539   |  
540   |  return max;
541   | }
542   |  
543   | static unsigned int find_dcfclk_for_voltage(const struct vg_dpm_clocks *clock_table,
544   |  unsigned int voltage)
545   | {
546   |  int i;
547   |  
548   |  for (i = 0; i < VG_NUM_SOC_VOLTAGE_LEVELS; i++) {
549   |  if (i >= VG_NUM_DCFCLK_DPM_LEVELS)
550   |  break;
551   |  if (clock_table->SocVoltage[i] == voltage)
552   |  return clock_table->DcfClocks[i];
    Loop bound 8 exceeds array 'DcfClocks' size 7; DcfClocks[i] may be out of bounds
553   | 	}
554   |  
555   |  ASSERT(0);
556   |  return 0;
557   | }
558   |  
559   | static void vg_clk_mgr_helper_populate_bw_params(
560   |  struct clk_mgr_internal *clk_mgr,
561   |  struct integrated_info *bios_info,
562   |  const struct vg_dpm_clocks *clock_table)
563   | {
564   |  int i, j;
565   |  struct clk_bw_params *bw_params = clk_mgr->base.bw_params;
566   |  
567   | 	j = -1;
568   |  
569   |  ASSERT(VG_NUM_FCLK_DPM_LEVELS <= MAX_NUM_DPM_LVL);
570   |  
571   |  /* Find lowest DPM, FCLK is filled in reverse order*/
572   |  
573   |  for (i = VG_NUM_FCLK_DPM_LEVELS - 1; i >= 0; i--) {
574   |  if (clock_table->DfPstateTable[i].fclk != 0) {
575   | 			j = i;
576   |  break;
577   | 		}
578   | 	}
579   |  
580   |  if (j == -1) {
581   |  /* clock table is all 0s, just use our own hardcode */
582   |  ASSERT(0);

Analysis:
- Decision: NotABug
- Reason: The pre-patch function had the target bug pattern: it looped over 8 `SocVoltage` entries and could index the 7-entry `DcfClocks` array at index 7. The reported source already contains the fix: it breaks when `i >= VG_NUM_DCFCLK_DPM_LEVELS` before comparing or indexing. Thus `DcfClocks[i]` is accessed only for indices 0 through 6, so the reported out-of-bounds access is not reachable.

# Checker
```cpp
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
#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Type.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/APSInt.h"
#include <string>

using namespace clang;
using namespace ento;
using namespace taint;

namespace {
/* The checker callbacks are to be decided. */
class SAGenTestChecker : public Checker<check::ASTCodeBody> {
   mutable std::unique_ptr<BugType> BT;

   public:
      SAGenTestChecker() : BT(new BugType(this, "Parallel-array index overflow", "Array bounds")) {}

      void checkASTCodeBody(const Decl *D, AnalysisManager &Mgr, BugReporter &BR) const;

   private:

      // Helpers for loop recognition and array access analysis
      static bool getCanonicalLoop(const ForStmt *FS,
                                   const VarDecl *&LoopVar,
                                   const Expr *&BoundExpr,
                                   bool &IsStrictLess,
                                   ASTContext &Ctx);

      static bool evalToInt(const Expr *E, llvm::APSInt &Out, ASTContext &Ctx);

      static bool indexIsLoopVar(const Expr *Idx, const VarDecl *V);

      static bool getArraySizeFromSubscriptBase(const Expr *Base, llvm::APInt &ArraySize, ASTContext &Ctx);

      static std::string getArrayName(const Expr *Base);

      static bool containsStmt(const Stmt *Root, const Stmt *Target);

      static bool isBreakGuard(const Stmt *S,
                               const VarDecl *LoopVar,
                               uint64_t ArraySize,
                               ASTContext &Ctx);

      static bool isProtectedByPriorBreakGuard(const ForStmt *FS,
                                               const ArraySubscriptExpr *ASE,
                                               const VarDecl *LoopVar,
                                               uint64_t ArraySize,
                                               ASTContext &Ctx);

      void report(const ArraySubscriptExpr *ASE,
                  uint64_t BoundVal,
                  StringRef ArrName,
                  uint64_t ArrSize,
                  BugReporter &BR,
                  ASTContext &Ctx) const;
};

//========================== Helper Implementations ==========================//

bool SAGenTestChecker::evalToInt(const Expr *E, llvm::APSInt &Out, ASTContext &Ctx) {
  if (!E)
    return false;
  Expr::EvalResult ER;
  if (E->EvaluateAsInt(ER, Ctx)) {
    Out = ER.Val.getInt();
    return true;
  }
  return false;
}

bool SAGenTestChecker::getCanonicalLoop(const ForStmt *FS,
                                        const VarDecl *&LoopVar,
                                        const Expr *&BoundExpr,
                                        bool &IsStrictLess,
                                        ASTContext &Ctx) {
  LoopVar = nullptr;
  BoundExpr = nullptr;
  IsStrictLess = true;

  if (!FS)
    return false;

  // 1) Init: either "int i = 0;" or "i = 0;"
  const Stmt *InitS = FS->getInit();
  const VarDecl *V = nullptr;

  if (const auto *DS = dyn_cast_or_null<DeclStmt>(InitS)) {
    if (!DS->isSingleDecl())
      return false;
    const auto *VD = dyn_cast<VarDecl>(DS->getSingleDecl());
    if (!VD || !VD->hasInit())
      return false;
    llvm::APSInt InitVal;
    if (!evalToInt(VD->getInit()->IgnoreParenImpCasts(), InitVal, Ctx))
      return false;
    if (InitVal != 0)
      return false;
    V = VD;
  } else if (const auto *BO = dyn_cast_or_null<BinaryOperator>(InitS)) {
    if (BO->getOpcode() != BO_Assign)
      return false;
    const auto *LHS = dyn_cast<DeclRefExpr>(BO->getLHS()->IgnoreParenImpCasts());
    if (!LHS)
      return false;
    const auto *VD = dyn_cast<VarDecl>(LHS->getDecl());
    if (!VD)
      return false;
    llvm::APSInt InitVal;
    if (!evalToInt(BO->getRHS()->IgnoreParenImpCasts(), InitVal, Ctx))
      return false;
    if (InitVal != 0)
      return false;
    V = VD;
  } else {
    return false;
  }

  // 2) Condition: "i < Bound" or "i <= Bound"
  const Expr *CondE = FS->getCond();
  if (!CondE)
    return false;
  CondE = CondE->IgnoreParenImpCasts();
  const auto *CBO = dyn_cast<BinaryOperator>(CondE);
  if (!CBO)
    return false;

  BinaryOperator::Opcode Op = CBO->getOpcode();
  if (Op != BO_LT && Op != BO_LE)
    return false;

  const auto *L = dyn_cast<DeclRefExpr>(CBO->getLHS()->IgnoreParenImpCasts());
  if (!L)
    return false;
  const auto *LVD = dyn_cast<VarDecl>(L->getDecl());
  if (!LVD || LVD != V)
    return false;

  IsStrictLess = (Op == BO_LT);
  BoundExpr = CBO->getRHS();

  // We do not strictly enforce increment pattern, as per plan.

  LoopVar = V;
  return true;
}

bool SAGenTestChecker::indexIsLoopVar(const Expr *Idx, const VarDecl *V) {
  if (!Idx || !V)
    return false;
  Idx = Idx->IgnoreParenImpCasts();
  if (const auto *DRE = dyn_cast<DeclRefExpr>(Idx)) {
    return DRE->getDecl() == V;
  }
  return false;
}

bool SAGenTestChecker::getArraySizeFromSubscriptBase(const Expr *Base, llvm::APInt &ArraySize, ASTContext &Ctx) {
  if (!Base)
    return false;

  // Case 1: direct DeclRefExpr to a variable with ConstantArrayType
  if (getArraySizeFromExpr(ArraySize, Base))
    return true;

  // Case 2: MemberExpr (struct or pointer-to-struct field)
  const MemberExpr *ME = dyn_cast<MemberExpr>(Base->IgnoreParenImpCasts());
  if (!ME) {
    // Try searching downward as a fallback
    ME = findSpecificTypeInChildren<MemberExpr>(Base);
  }
  if (ME) {
    if (const auto *FD = dyn_cast<FieldDecl>(ME->getMemberDecl())) {
      QualType T = FD->getType();
      if (const auto *CAT = dyn_cast<ConstantArrayType>(T.getTypePtr())) {
        ArraySize = CAT->getSize();
        return true;
      }
    }
  }

  // Unknown or pointer-based indexing: skip
  return false;
}

std::string SAGenTestChecker::getArrayName(const Expr *Base) {
  if (!Base)
    return std::string();

  Base = Base->IgnoreParenImpCasts();

  if (const auto *DRE = dyn_cast<DeclRefExpr>(Base)) {
    if (const auto *VD = dyn_cast<ValueDecl>(DRE->getDecl()))
      return VD->getNameAsString();
  }

  if (const auto *ME = dyn_cast<MemberExpr>(Base)) {
    if (const auto *VD = dyn_cast<ValueDecl>(ME->getMemberDecl()))
      return VD->getNameAsString();
  }

  // Fallback: try to find a nested MemberExpr
  if (const auto *ME2 = findSpecificTypeInChildren<MemberExpr>(Base)) {
    if (const auto *VD = dyn_cast<ValueDecl>(ME2->getMemberDecl()))
      return VD->getNameAsString();
  }

  return std::string();
}

bool SAGenTestChecker::containsStmt(const Stmt *Root, const Stmt *Target) {
  if (!Root || !Target)
    return false;
  if (Root == Target)
    return true;

  for (const Stmt *Child : Root->children()) {
    if (containsStmt(Child, Target))
      return true;
  }
  return false;
}

bool SAGenTestChecker::isBreakGuard(const Stmt *S,
                                    const VarDecl *LoopVar,
                                    uint64_t ArraySize,
                                    ASTContext &Ctx) {
  const auto *IS = dyn_cast_or_null<IfStmt>(S);
  if (!IS || !IS->getCond() || !IS->getThen())
    return false;

  // Only recognize a direct break (optionally wrapped in a one-statement
  // compound), so a nested break in a switch or another loop is not mistaken
  // for a guard on this loop.
  const Stmt *Then = IS->getThen();
  if (const auto *CS = dyn_cast<CompoundStmt>(Then)) {
    if (CS->size() != 1)
      return false;
    Then = *CS->body_begin();
  }
  if (!isa<BreakStmt>(Then))
    return false;

  const auto *Cond = dyn_cast<BinaryOperator>(
      IS->getCond()->IgnoreParenImpCasts());
  if (!Cond || Cond->getOpcode() != BO_GE)
    return false;

  const auto *LHS = dyn_cast<DeclRefExpr>(
      Cond->getLHS()->IgnoreParenImpCasts());
  if (!LHS || LHS->getDecl() != LoopVar)
    return false;

  llvm::APSInt GuardBound;
  if (!evalToInt(Cond->getRHS()->IgnoreParenImpCasts(), GuardBound, Ctx) ||
      (GuardBound.isSigned() && GuardBound.isNegative()))
    return false;

  const uint64_t GuardLimit =
      GuardBound.isSigned() ? static_cast<uint64_t>(GuardBound.getExtValue())
                            : GuardBound.getZExtValue();

  // If i >= GuardLimit breaks, indices reaching GuardLimit are not accessed.
  return GuardLimit <= ArraySize;
}

bool SAGenTestChecker::isProtectedByPriorBreakGuard(
    const ForStmt *FS,
    const ArraySubscriptExpr *ASE,
    const VarDecl *LoopVar,
    uint64_t ArraySize,
    ASTContext &Ctx) {
  if (!FS || !ASE)
    return false;

  const auto *Body = dyn_cast_or_null<CompoundStmt>(FS->getBody());
  if (!Body)
    return false;

  bool HasPriorGuard = false;
  for (const Stmt *S : Body->body()) {
    // The guard must be a preceding statement. A guard nested in the same
    // statement as the access is deliberately not treated as dominating it.
    if (containsStmt(S, ASE))
      return HasPriorGuard;

    if (isBreakGuard(S, LoopVar, ArraySize, Ctx))
      HasPriorGuard = true;
  }

  return false;
}

void SAGenTestChecker::report(const ArraySubscriptExpr *ASE,
                              uint64_t BoundVal,
                              StringRef ArrName,
                              uint64_t ArrSize,
                              BugReporter &BR,
                              ASTContext &Ctx) const {
  if (!ASE)
    return;

  SmallString<128> Msg;
  llvm::raw_svector_ostream OS(Msg);
  OS << "Loop bound " << BoundVal << " exceeds array '" << ArrName
     << "' size " << ArrSize << "; " << ArrName << "[i] may be out of bounds";

  PathDiagnosticLocation Loc(ASE->getBeginLoc(), BR.getSourceManager());

  auto R = std::make_unique<BasicBugReport>(*BT, OS.str(), Loc);
  R->addRange(ASE->getSourceRange());
  BR.emitReport(std::move(R));
}

//============================ Main AST Callback =============================//

void SAGenTestChecker::checkASTCodeBody(const Decl *D, AnalysisManager &Mgr, BugReporter &BR) const {
  if (!D)
    return;
  const Stmt *Body = D->getBody();
  if (!Body)
    return;

  ASTContext &Ctx = Mgr.getASTContext();

  // Visitor to find ForStmt and analyze them.
  class Visitor : public RecursiveASTVisitor<Visitor> {
    const SAGenTestChecker *Checker;
    BugReporter &BR;
    ASTContext &Ctx;

  public:
    Visitor(const SAGenTestChecker *Checker, BugReporter &BR, ASTContext &Ctx)
        : Checker(Checker), BR(BR), Ctx(Ctx) {}

    bool VisitForStmt(const ForStmt *FS) {
      const VarDecl *LoopVar = nullptr;
      const Expr *BoundExpr = nullptr;
      bool IsStrictLess = true;

      if (!SAGenTestChecker::getCanonicalLoop(FS, LoopVar, BoundExpr, IsStrictLess, Ctx))
        return true;

      llvm::APSInt BoundAPS;
      if (!SAGenTestChecker::evalToInt(BoundExpr->IgnoreParenImpCasts(), BoundAPS, Ctx))
        return true;

      uint64_t BoundVal = BoundAPS.isSigned() ? static_cast<uint64_t>(BoundAPS.getExtValue()) : BoundAPS.getZExtValue();
      // We only handle non-negative bounds
      if ((BoundAPS.isSigned() && BoundAPS.isNegative()))
        return true;

      // Collect array subscripts with index equal to the loop variable
      class BodyVisitor : public RecursiveASTVisitor<BodyVisitor> {
        const VarDecl *V;
        llvm::SmallVector<const ArraySubscriptExpr *, 8> &Out;
      public:
        BodyVisitor(const VarDecl *V, llvm::SmallVector<const ArraySubscriptExpr *, 8> &Out)
            : V(V), Out(Out) {}

        bool VisitArraySubscriptExpr(const ArraySubscriptExpr *ASE) {
          if (!ASE)
            return true;
          const Expr *Idx = ASE->getIdx();
          if (SAGenTestChecker::indexIsLoopVar(Idx, V)) {
            Out.push_back(ASE);
          }
          return true;
        }
      };

      llvm::SmallVector<const ArraySubscriptExpr *, 8> Accesses;
      BodyVisitor BV(LoopVar, Accesses);
      if (const Stmt *LoopBody = FS->getBody())
        BV.TraverseStmt(const_cast<Stmt *>(LoopBody));

      // Report per array per loop (avoid duplicates)
      llvm::SmallPtrSet<const ValueDecl *, 8> Reported;

      for (const ArraySubscriptExpr *ASE : Accesses) {
        if (!ASE)
          continue;

        llvm::APInt ArrSizeAP;
        if (!SAGenTestChecker::getArraySizeFromSubscriptBase(ASE->getBase(), ArrSizeAP, Ctx))
          continue;

        uint64_t ArrSize = ArrSizeAP.getLimitedValue(UINT64_MAX);

        if (SAGenTestChecker::isProtectedByPriorBreakGuard(
                FS, ASE, LoopVar, ArrSize, Ctx))
          continue;

        bool IsBug = false;
        if (IsStrictLess) {
          // for (i = 0; i < Bound) accessing A[i]: overflow if Bound > ArrSize
          if (BoundVal > ArrSize)
            IsBug = true;
        } else {
          // for (i = 0; i <= Bound) accessing A[i]: overflow if Bound >= ArrSize
          if (BoundVal >= ArrSize)
            IsBug = true;
        }

        if (!IsBug)
          continue;

        // Identify the array's ValueDecl to deduplicate
        const ValueDecl *VDKey = nullptr;
        const Expr *Base = ASE->getBase()->IgnoreParenImpCasts();
        if (const auto *DRE = dyn_cast<DeclRefExpr>(Base)) {
          VDKey = dyn_cast<ValueDecl>(DRE->getDecl());
        } else if (const auto *ME = dyn_cast<MemberExpr>(Base)) {
          VDKey = dyn_cast<ValueDecl>(ME->getMemberDecl());
        } else if (const auto *ME2 = findSpecificTypeInChildren<MemberExpr>(ASE->getBase())) {
          VDKey = dyn_cast<ValueDecl>(ME2->getMemberDecl());
        }

        if (VDKey && Reported.contains(VDKey))
          continue;
        if (VDKey)
          Reported.insert(VDKey);

        std::string Name = SAGenTestChecker::getArrayName(ASE->getBase());
        Checker->report(ASE, BoundVal, Name.empty() ? StringRef("array") : StringRef(Name),
                        ArrSize, BR, Ctx);
      }

      return true;
    }
  };

  Visitor V(this, BR, Ctx);
  V.TraverseStmt(const_cast<Stmt *>(Body));
}

} // end anonymous namespace

extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<SAGenTestChecker>(
      "custom.SAGenTestChecker",
      "Detect loops that index into a smaller parallel array using a larger loop bound",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;


```

# Formatting

Please provide the whole checker code after fixing the false positive.
The refined code must be surrounded by ```cpp and ```.
Your response should be like:

Refinment Plan:
XXX

Refined Code:
```cpp
{{fixed checker code here}}
```
