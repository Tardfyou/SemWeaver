You are in the `decide` phase of detector refinement.

Goal: identify the smallest evidence-backed edit that keeps the motivating
vulnerable target while removing fixed-side alerts in the same patch-related
scope. Choose whether to finish, request evidence, edit, or validate. A scoped
rule still needs a real mechanism predicate, not merely a patch spelling.

{{TASK_PROMPT}}

Current turn: {{ITERATION}} / {{MAX_ITERATIONS}}

System behavior:
- The current working artifact has already been read.
- Any baseline quality result appears in Additional context.
- After `apply_patch`, the system applies exact snippet replacements to the
  latest artifact and returns to decide; it does not validate immediately.
- You may apply multiple coherent edits until this round's semantic model is
  complete, then output `validate`.
- CSA validation runs LSP, review, compilation, and functional validation.
  CodeQL validation runs review and analysis.
- Once validation begins, failures go only to repair for the smallest correction.
- After every gate passes, the validated artifact becomes the next-round baseline.
  Output `finish` when that new baseline is already sufficient.
- Do not preemptively repair validation errors that have not occurred.
- In E2/KNighter runs, `request_evidence` only selects records already attached
  to this frozen bundle. It never performs new collection.
- If Additional context already contains `evidence:*`,
  `request_evidence.budget`, or `request_evidence.exhausted`, do not repeat the
  same request. Use the patch, source slice, validation result, available
  evidence, or a targeted source read.
- Evidence origin labels are part of the experiment contract. Never present a
  source-derived, behavioral, or human/backfilled record as analyzer-internal.

Current working artifact:
```text
{{ARTIFACT_TEXT}}
```

Patch:
```diff
{{PATCH_TEXT}}
```

Additional context:
{{CONTEXT_NOTES}}

---

## Available evidence types

Request only the most important missing semantic evidence:

| Evidence type | Meaning | Typical use |
|---|---|---|
| `patch_fact` | structured patch facts | bug type, fix pattern, affected functions |
| `semantic_slice` | patch-related semantic slice | surrounding functions and statements |
| `dataflow_candidate` | candidate flows | source/sink and value/state propagation |
| `call_chain` | caller/callee relations | interprocedural propagation |
| `path_guard` | path guards | added checks, bounds, and barriers |
| `allocation_lifecycle` | allocation/ownership lifecycle | UAF, double-free, and leak mechanisms |
| `state_transition` | abstract state transitions | locks, refcounts, state machines, flags |
| `directory_tree` | directory structure | locating relevant source files |

Never request evidence already supplied. If the frozen bundle lacks a requested
type, repeating the request cannot create it. Read a specific reference file or
choose edit, validate, or finish. If the run requires an analyzer-internal origin
and it is unavailable, do not compensate by inventing a source-derived fact; the
outer provenance gate will abstain.

## Decide-phase reasoning

Perform these checks internally. Keep `summary` to one short sentence.

### Step 1: Is the current implementation already sufficient?
- Did baseline review pass?
- If this is the next decide after a passing round, finish when the new baseline
  needs no evidence-backed improvement.
- Does the checker/query cover the patch's guard, state, ownership, capacity,
  or value relation rather than only an API name or exact statement order?
- Is any proposed scope restriction justified by the patch-related validation
  target and independent of the semantic warning predicate?
- Would another edit merely create an equivalent diff or return to heuristics?
- Is the current abstraction so wrong that local edits cannot repair it?
- At minimum, does vulnerable code warn while fixed code remains silent?

### Step 2: What is missing?
- Does the detector rely on API names, source strings, `strlen/strnlen`, or
  variables named `len/size/bytes`, or merely replace a broad name heuristic
  with a narrower one? That is not mechanism modeling.
- Would adding an unrelated statement or changing a local name defeat the
  predicate without fixing the bug? If so, revise it when the current call
  budget permits; otherwise record this as a post-freeze robustness risk.
- Does it distinguish the paired patch mechanism, rather than claiming to
  detect a whole vulnerability family from this one example?
- Did the patch add a guard, capacity relation, state constraint, barrier,
  ownership/lifetime transition, numeric-domain fix, or authoritative relookup?
- Identify the needed mechanism roles: trigger, intermediate state or quantity,
  object relation, update/propagation, guard/barrier, sink, invalidating
  condition, and silence condition.
- Add only roles needed to explain the observed fixed-side noise. Preserve
  compiling callbacks and helper APIs wherever possible.

### Step 2.5: Check the mechanism chain
Before acting, express the minimum chain:

precondition -> key value/state/object relation -> update or propagation ->
guard/barrier/widening/ownership/capacity relation -> dangerous use or sink ->
patch-introduced fix.

Check which roles the current detector implements. A local proxy is insufficient
if it explains why one site warns but not why a surface-similar safe site should
remain silent.

### Step 3: Choose an action
- `request_evidence`: request the 1--3 most important missing evidence types.
- `apply_patch`: evidence is sufficient, but the mechanism model is incomplete;
  submit a coherent edit and continue decide.
- `validate`: the mechanism chain is complete and only implementation risk remains.
- `finish`: the artifact is already sufficient or no safe high-value edit exists.
  Do not use finish as a substitute for validation.

## Edit constraints
- `apply_patch` always uses exact snippet replacement.
- Every edit must target the latest artifact and match uniquely.
- Multiple edits in one response must form one coherent semantic change and
  remain valid sequentially.
- Except for `edits`, keep string fields to one short sentence.
- A large single-file rewrite is allowed only when the existing abstraction is
  demonstrably unusable, and it must still use exact replacements.
- Do not split a supported semantic change into token-sized edits merely to
  reach validation sooner.

CSA constraints:
- Do not invent APIs or helpers whose signatures are not confirmed by the
  artifact or references.
- For an explicit length check plus bounded-API patch, model missing guard and
  capacity proof rather than an API blacklist.
- Callee-only reports, variable-name heuristics, fake state, and exact
  cleanup-label matches remain unacceptable as the sole warning predicate.
  Use the known patch-related function to locate source context; do not add an
  exact `checkASTCodeBody` function-name filter, which the review gate blocks.
- Use `eval::Assume` with `evalAssume(...)` for Clang~18 path assumptions; do
  not invent `check::Assume` or `checkAssume(...)`.
- `checkBranchCondition` receives a `const Stmt *`; cast it to `const Expr *`
  before calling `IgnoreParenImpCasts()`. `ProgramState` has no direct
  `getSymVal(SymbolRef)` in this toolchain; use the `ConstraintManager` form
  already specified below.
- For an address-taken output such as `&fw`, obtain the output-slot region from
  the original address expression. Passing the `fw` value expression obtained
  from `getSubExpr()` to `getMemRegionFromExpr` can yield no region and silently
  disable all tracking.
- A widening consumer can be a `VarDecl` initializer (`u64 out = narrow_shift`),
  not only a parent `Expr`; include declaration parents in width-flow checks.
- Normalize negated call conditions before interpreting success/failure
  branches. For `if (!register(...)) success; else failure;`, the registration
  call is below `!` and the failure branch is the `else` arm.
- An off-by-one guard is only unsafe relative to a downstream indexed target.
  Bind the guarded value, index offset, and destination capacity; the guard's
  source array length alone is not a vulnerability predicate.
- A `checkBranchCondition` callback that marks an output valid merely because a
  status variable appears in the condition is unsound: it clears both
  successors. For status-zero success contracts, retain the return `SymbolRef`
  and clear only in `evalAssume` when `getSymVal` proves zero.

CodeQL constraints:
- Declare quantified variables in the same `exists()`.
- Do not invent types, members, dominance, or guard APIs.
- Reuse APIs demonstrated in successful repository queries. In particular, do
  not invent `IntegerLiteral` or `IntLiteral` types.

Return exactly one JSON object, with no Markdown or commentary.
Field order: `action`, `summary`, `evidence_types`, `edits`.

Rules:
- Do not add fields.
- `request_evidence`: provide 1--3 `evidence_types`; `edits` is empty.
- `apply_patch`: provide `edits`; `evidence_types` is empty.
- `validate` or `finish`: both arrays are empty.

JSON schema:
{
  "action": "request_evidence" | "apply_patch" | "validate" | "finish",
  "summary": "One short sentence",
  "evidence_types": ["1-3 evidence types only for request_evidence"],
  "edits": [
    {
      "old_snippet": "A uniquely matching snippet from the current artifact",
      "new_snippet": "The replacement"
    }
  ]
}
