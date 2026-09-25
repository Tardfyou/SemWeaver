Analyzer: {{ANALYZER_ID}}
Analyzer name: {{ANALYZER_NAME}}
Working directory: {{WORK_DIR}}
Patch: {{PATCH_PATH}}
Validation target: {{VALIDATE_PATH}}
Maximum model turns: {{MAX_ITERATIONS}}

Task: produce one stable detector/query for this analyzer within the fixed workflow.

Hard generation constraints:
- Build the minimum trigger from patch files, functions, and nearby AST, call,
  assignment, state, and release facts.
- First obtain a patch-local hit; only then add guards, barriers, dominance, or
  data-flow filters.
- A first draft may be specialized, but it must remain grounded in observable
  statements near the change. More hits are preferable to zero hits.
- Function anchors must follow the actual patch hunk and validation source.
  Field names or caller-flow functions are supporting evidence, not substitutes
  for the changed target.
- For a deleted call/release/write, first match the removed operation, then
  inspect earlier/later release, use, reassign, null, and guard events on the same
  resource in the changed function. Do not assume interprocedural ownership.
- For an added guard/barrier, first match the source/sink that was previously
  reachable; the first draft need not encode every fixed-side exclusion.
- Cover assignment and declaration initialization, plus `=`, `|=`, `+=`, and
  other relevant compound writes.
- Do not let complex data flow, dominance, RAG, or generalization constraints
  eliminate the patch-local trigger.

Analyzer policy:
{{ANALYZER_POLICY}}
