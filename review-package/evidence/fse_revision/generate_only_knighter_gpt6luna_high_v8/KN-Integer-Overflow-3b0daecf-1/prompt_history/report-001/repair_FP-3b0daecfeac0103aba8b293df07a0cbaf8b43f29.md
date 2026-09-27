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

Manually multiplying count by element size when allocating an array with kmalloc/kzalloc:
ptr = kzalloc(count * sizeof(*ptr), GFP_KERNEL);
This risks integer overflow in the size calculation, leading to undersized allocations and subsequent out-of-bounds writes/reads. Use kcalloc(count, sizeof(*ptr), GFP_KERNEL) which performs overflow checking.


The patch that needs to be detected:

## Patch Description

amdkfd: use calloc instead of kzalloc to avoid integer overflow

This uses calloc instead of doing the multiplication which might
overflow.

Cc: stable@vger.kernel.org
Signed-off-by: Dave Airlie <airlied@redhat.com>

## Buggy Code

```c
// Function: kfd_ioctl_get_process_apertures_new in drivers/gpu/drm/amd/amdkfd/kfd_chardev.c
static int kfd_ioctl_get_process_apertures_new(struct file *filp,
				struct kfd_process *p, void *data)
{
	struct kfd_ioctl_get_process_apertures_new_args *args = data;
	struct kfd_process_device_apertures *pa;
	int ret;
	int i;

	dev_dbg(kfd_device, "get apertures for PASID 0x%x", p->pasid);

	if (args->num_of_nodes == 0) {
		/* Return number of nodes, so that user space can alloacate
		 * sufficient memory
		 */
		mutex_lock(&p->mutex);
		args->num_of_nodes = p->n_pdds;
		goto out_unlock;
	}

	/* Fill in process-aperture information for all available
	 * nodes, but not more than args->num_of_nodes as that is
	 * the amount of memory allocated by user
	 */
	pa = kzalloc((sizeof(struct kfd_process_device_apertures) *
				args->num_of_nodes), GFP_KERNEL);
	if (!pa)
		return -ENOMEM;

	mutex_lock(&p->mutex);

	if (!p->n_pdds) {
		args->num_of_nodes = 0;
		kfree(pa);
		goto out_unlock;
	}

	/* Run over all pdd of the process */
	for (i = 0; i < min(p->n_pdds, args->num_of_nodes); i++) {
		struct kfd_process_device *pdd = p->pdds[i];

		pa[i].gpu_id = pdd->dev->id;
		pa[i].lds_base = pdd->lds_base;
		pa[i].lds_limit = pdd->lds_limit;
		pa[i].gpuvm_base = pdd->gpuvm_base;
		pa[i].gpuvm_limit = pdd->gpuvm_limit;
		pa[i].scratch_base = pdd->scratch_base;
		pa[i].scratch_limit = pdd->scratch_limit;

		dev_dbg(kfd_device,
			"gpu id %u\n", pdd->dev->id);
		dev_dbg(kfd_device,
			"lds_base %llX\n", pdd->lds_base);
		dev_dbg(kfd_device,
			"lds_limit %llX\n", pdd->lds_limit);
		dev_dbg(kfd_device,
			"gpuvm_base %llX\n", pdd->gpuvm_base);
		dev_dbg(kfd_device,
			"gpuvm_limit %llX\n", pdd->gpuvm_limit);
		dev_dbg(kfd_device,
			"scratch_base %llX\n", pdd->scratch_base);
		dev_dbg(kfd_device,
			"scratch_limit %llX\n", pdd->scratch_limit);
	}
	mutex_unlock(&p->mutex);

	args->num_of_nodes = i;
	ret = copy_to_user(
			(void __user *)args->kfd_process_device_apertures_ptr,
			pa,
			(i * sizeof(struct kfd_process_device_apertures)));
	kfree(pa);
	return ret ? -EFAULT : 0;

out_unlock:
	mutex_unlock(&p->mutex);
	return 0;
}
```

## Bug Fix Patch

```diff
diff --git a/drivers/gpu/drm/amd/amdkfd/kfd_chardev.c b/drivers/gpu/drm/amd/amdkfd/kfd_chardev.c
index f9631f4b1a02..55aa74cbc532 100644
--- a/drivers/gpu/drm/amd/amdkfd/kfd_chardev.c
+++ b/drivers/gpu/drm/amd/amdkfd/kfd_chardev.c
@@ -779,8 +779,8 @@ static int kfd_ioctl_get_process_apertures_new(struct file *filp,
 	 * nodes, but not more than args->num_of_nodes as that is
 	 * the amount of memory allocated by user
 	 */
-	pa = kzalloc((sizeof(struct kfd_process_device_apertures) *
-				args->num_of_nodes), GFP_KERNEL);
+	pa = kcalloc(args->num_of_nodes, sizeof(struct kfd_process_device_apertures),
+		     GFP_KERNEL);
 	if (!pa)
 		return -ENOMEM;

```


# False Positive Report

BuildSource:| drivers/gpu/drm/amd/amdkfd/kfd_chardev.c
### Report Summary

File:|
/work/SemWeaver/artifacts/external/linux/drivers/gpu/drm/amd/amdgpu/../amdkfd/kfd_chardev.c  
---|---  
Warning:| line 1891, column 15  
Use kcalloc(count, size, ...) instead of count*sizeof in allocation to avoid
integer overflow  
  
### Annotated Source Code


1830  |  return ret;
1831  | }
1832  |  
1833  | static uint32_t get_process_num_bos(struct kfd_process *p)
1834  | {
1835  | 	uint32_t num_of_bos = 0;
1836  |  int i;
1837  |  
1838  |  /* Run over all PDDs of the process */
1839  |  for (i = 0; i < p->n_pdds; i++) {
1840  |  struct kfd_process_device *pdd = p->pdds[i];
1841  |  void *mem;
1842  |  int id;
1843  |  
1844  |  idr_for_each_entry(&pdd->alloc_idr, mem, id) {
1845  |  struct kgd_mem *kgd_mem = (struct kgd_mem *)mem;
1846  |  
1847  |  if (!kgd_mem->va || kgd_mem->va > pdd->gpuvm_base)
1848  | 				num_of_bos++;
1849  | 		}
1850  | 	}
1851  |  return num_of_bos;
1852  | }
1853  |  
1854  | static int criu_get_prime_handle(struct kgd_mem *mem,
1855  |  int flags, u32 *shared_fd)
1856  | {
1857  |  struct dma_buf *dmabuf;
1858  |  int ret;
1859  |  
1860  | 	ret = amdgpu_amdkfd_gpuvm_export_dmabuf(mem, &dmabuf);
1861  |  if (ret) {
1862  |  pr_err("dmabuf export failed for the BO\n");
1863  |  return ret;
1864  | 	}
1865  |  
1866  | 	ret = dma_buf_fd(dmabuf, flags);
1867  |  if (ret < 0) {
1868  |  pr_err("dmabuf create fd failed, ret:%d\n", ret);
1869  |  goto out_free_dmabuf;
1870  | 	}
1871  |  
1872  | 	*shared_fd = ret;
1873  |  return 0;
1874  |  
1875  | out_free_dmabuf:
1876  | 	dma_buf_put(dmabuf);
1877  |  return ret;
1878  | }
1879  |  
1880  | static int criu_checkpoint_bos(struct kfd_process *p,
1881  | 			       uint32_t num_bos,
1882  | 			       uint8_t __user *user_bos,
1883  | 			       uint8_t __user *user_priv_data,
1884  | 			       uint64_t *priv_offset)
1885  | {
1886  |  struct kfd_criu_bo_bucket *bo_buckets;
1887  |  struct kfd_criu_bo_priv_data *bo_privs;
1888  |  int ret = 0, pdd_index, bo_index = 0, id;
1889  |  void *mem;
1890  |  
1891  |  bo_buckets = kvzalloc(num_bos * sizeof(*bo_buckets), GFP_KERNEL);
    Use kcalloc(count, size, ...) instead of count*sizeof in allocation to avoid integer overflow
1892  |  if (!bo_buckets)
1893  |  return -ENOMEM;
1894  |  
1895  | 	bo_privs = kvzalloc(num_bos * sizeof(*bo_privs), GFP_KERNEL);
1896  |  if (!bo_privs) {
1897  | 		ret = -ENOMEM;
1898  |  goto exit;
1899  | 	}
1900  |  
1901  |  for (pdd_index = 0; pdd_index < p->n_pdds; pdd_index++) {
1902  |  struct kfd_process_device *pdd = p->pdds[pdd_index];
1903  |  struct amdgpu_bo *dumper_bo;
1904  |  struct kgd_mem *kgd_mem;
1905  |  
1906  |  idr_for_each_entry(&pdd->alloc_idr, mem, id) {
1907  |  struct kfd_criu_bo_bucket *bo_bucket;
1908  |  struct kfd_criu_bo_priv_data *bo_priv;
1909  |  int i, dev_idx = 0;
1910  |  
1911  |  if (!mem) {
1912  | 				ret = -ENOMEM;
1913  |  goto exit;
1914  | 			}
1915  |  
1916  | 			kgd_mem = (struct kgd_mem *)mem;
1917  | 			dumper_bo = kgd_mem->bo;
1918  |  
1919  |  /* Skip checkpointing BOs that are used for Trap handler
1920  |  * code and state. Currently, these BOs have a VA that
1921  |  * is less GPUVM Base

Analysis:
I’ll trace how `num_bos` is computed and consumed in the CRIU checkpoint path, then compare the `kvzalloc` allocation against the target’s overflow pattern and patch rationale. The key question is whether the count can reach a value that wraps the allocation size before the later BO writes.The report flags the same arithmetic shape, but on a separate allocation in CRIU checkpointing. I’m checking the caller-side count and the actual indexing/copy behavior to distinguish a stylistic match from a reachable undersized allocation.- Decision: NotABug
- Reason: The report matches the unchecked `count * sizeof(*ptr)` pattern: `kvzalloc` does not check for overflow in an already-computed size, and `kcalloc` would be the analogous fix. However, `num_bos` is a `uint32_t`, and KFD is supported on 64-bit architectures. There, the multiplication is performed as `size_t`; even at `UINT32_MAX`, multiplying by the size of either struct cannot overflow 64-bit `size_t`. The allocation may fail for a large count, but that returns `-ENOMEM` rather than creating an undersized buffer. So this instance does not demonstrate a reachable overflow bug.

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
#include "clang/Lex/Lexer.h"

using namespace clang;
using namespace ento;
using namespace taint;

// No custom program state needed.

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
  bool HasSizeof = exprContainsSizeof(LHS) || exprContainsSizeof(RHS);
  if (!HasSizeof)
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
