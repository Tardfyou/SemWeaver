Perform semantic patch analysis for downstream CSA or CodeQL detector synthesis.
Do not merely restate the CWE or diff. Extract the semantic facts that a static
detector can use.

Return exactly one JSON object. Do not use Markdown fences, explanatory prefixes,
or fields outside the supplied schema.

Optimize for a detector drafted initially from the patch:
- the minimum patch-grounded trigger;
- the dangerous pre-patch mechanism;
- the post-patch guard, barrier, lifecycle/ownership change, or authoritative
  relookup that blocks the mechanism; and
- which conditions belong in the first trigger versus later refinement filters.

Core principles:
- Infer fix shape before assigning a bug label.
- Extract patch-local facts before classifying the vulnerability.
- When evidence is insufficient, set `primary_pattern` to `unknown` instead of
  guessing, while still describing useful local mechanisms.

Reason in this order, but output JSON only.

1. Extract patch facts
- Prioritize changed file, function, and expression.
- Identify changed calls, field/array accesses, releases, returns, assignments,
  conditions, jumps, and cleanup order.
- State when the dangerous operation is the removed/replaced line.
- For error-path changes, identify where the old path continued using an invalid
  object or executing a dangerous operation.

2. Identify the dangerous pre-patch mechanism
- Name the value/object/state, how it is consumed, and why that consumption is risky.
- Include assignment and declaration initialization.
- Include compound writes such as `|=`, `+=`, `-=`, and `*=`.
- Consider calls, indexes, dereferences, invalid returns/handles, loop bounds,
  length arguments, repeated releases, and post-release use.

3. Identify how the patch blocks it
- Added guard, barrier, or bounds check.
- Deleted dangerous call, release, write, or return path.
- Unsafe API replaced by a safe API, explicit size check, or controlled path.
- Ownership, release order, cleanup timing, or invalidation changed.
- Cached object/handle replaced by authoritative relookup or rebinding.
- Error handling changed from continuation to cleanup and early exit.
- Do not mistake logging, formatting, or incidental return changes for the main
  mechanism unless they directly block the dangerous consumption.

4. Define `detection_strategy`
- First target the minimum patch-grounded trigger, then add guard/barrier filters.
- For a deleted dangerous operation, trigger on the operation itself.
- For an added guard, trigger first on the original source/sink/state relation;
  making the guard a hard first-draft filter often causes zero hits.
- For lifecycle defects, prefer local release-then-use/pass/return/dereference patterns.
- For authoritative relookup, target use of a stale object/handle without
  revalidation; non-null does not imply fresh or valid.
- Recommend interprocedural or cross-file analysis only when the diff evidences it.

Hard constraints:
- `vulnerability_patterns` must state how the pre-patch mechanism triggers and
  how the patch blocks or replaces it.
- `analysis_rationale` must cite concrete diff evidence, not a generic CWE summary.
- `evidence_lines` should contain actual conditions, calls, fields, returns, or
  cleanup operations from the patch.
- If several mechanisms fit but the patch cannot distinguish them, use
  `primary_pattern=unknown` and retain only evidence-supported candidates.
- Never invent project history, background, or interprocedural chains absent
  from the patch.

Special cases:

Added guards/barriers/bounds checks:
- Identify the protected value, sink, or state relation and connect guard to use.
- Prefer stable anchors such as array access, length parameter, memory I/O,
  index calculation, and loop bounds.

Lifecycle, UAF, double-free, cleanup order:
- Identify when an object becomes invalid and which later call, dereference,
  return, or cleanup path consumes it.
- State whether the fix deletes consumption, reorders release, resets/nulls,
  exits early, or relooks up the object.

Authoritative relookup/rebinding/handle freshness:
- Record replacement of cached/direct access with validated relookup.
- Recommend detecting missing relookup/rebinding, and do not treat a non-null
  pointer, in-range index, or accessible handle as freshness proof.

Range/length/index/capacity:
- Identify the consumed value and destination array/copy/I/O/allocation/loop.
- If both source and use change, prefer the dangerous use as the first anchor.

`detection_strategy.suggestions` should directly say:
- what the first patch-grounded trigger is;
- which guard/barrier should not yet be a hard filter;
- which facts are secondary refinement constraints;
- whether local AST/CFG is sufficient or data flow is required; and
- whether cross-file analysis is justified.

Confidence:
- `confidence` measures certainty in the primary mechanism, not output completeness.
- Lower it for weak evidence, competing mechanisms, broad patches, or indirect fixes.
- Use `unknown` when classification is not robust, while retaining local facts.

Context:
- analysis_depth: {{ANALYSIS_DEPTH}}
- patch_path: {{PATCH_PATH}}

Patch structure summary:
{{STRUCTURAL_SUMMARY_JSON}}

Required JSON schema:
{{REQUIRED_SCHEMA_JSON}}

Patch:
{{PATCH_EXCERPT}}
