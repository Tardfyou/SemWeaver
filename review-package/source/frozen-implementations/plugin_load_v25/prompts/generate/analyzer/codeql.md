CodeQL objective: generate a stable, parseable, executable `.ql` query whose
patch-differential semantics are explicit.

Core goals:
- Match a real vulnerable pattern near changed files/functions/statements.
- Establish the patch-grounded trigger before adding flow, dominance, or call-
  chain filters.
- A first draft may be specialized, but exact file/function/literal/name matches
  cannot be the sole semantic condition.
- Narrowing conditions must be observable in the patch or validation source.

Modeling priorities:
1. Classify the patch: guard addition, dangerous-operation deletion, API
   replacement, state/ownership change, range/length change, or other.
2. Establish the trigger from changed expressions, variables/fields/APIs, and
   nearby same-function events.
3. For a deleted operation, trigger on that operation; do not invent
   `hasPatchGuard`.
4. For an added guard/barrier, first match the original source/sink.
5. For API replacement, match old API and argument roles, then model silence via
   disappearance or safe replacement.
6. If a parse/read/assignment touchpoint is the stable vulnerable anchor, report
   it directly instead of forcing a nonexistent missing-guard AST shape.
7. Cover assignment and declaration initialization plus compound writes.

Deleted release/destroy patches:
- Search earlier/later same-resource events in the changed function by source
  order. If the removed call may be the second release, search earlier.
- Do not require a failure callee to receive the resource unless its arguments do.
- Extract variable/member accesses from the release argument subtree rather than
  requiring the argument itself to be a `VariableAccess`.

CodeQL constraints:
- Run `generate_codeql_query` before materializing the first draft.
- Include `@name`, `@description`, `@kind problem`, `@problem.severity`,
  `@precision`, and a unique stable `@id` such as
  `cpp/custom/<checker-name-kebab>`.
- Do not invent CodeQL APIs, types, members, or AST/DataFlow names.
- Declare quantified variables in the same `exists()`; never reference sibling
  quantifier locals.
- Use `getEnclosingFunction()` only on AST types that support it.
- Do not treat `getParent*()` as a general containment proof. Traverse from
  concrete structural entry points such as conditions, loop controllers, array
  offsets, and binary operands.
- Do not use `toString()` as the main semantic test. Compare variable and field
  declaration identities with `getTarget()`.
- `FieldAccess` has no `getField()`; use `getTarget()`.
- Do not assume `FunctionCall`/`Call` has `getNumArgument()`. Access known slots
  or verify a concrete available API first.
- Match patch-local dangerous calls and argument roles before uncertain higher-
  order call APIs.
- Descendant relations such as `getAChild*()` are supporting conditions, not the
  only proof.

Recommended layering:
```ql
predicate inPatchScope(Element e) { ... }
predicate isPatchGroundedTrigger(Element anchor) { ... }
predicate fixedHasBarrier(Element anchor) { ... } // only when evidenced

from Element anchor
where inPatchScope(anchor)
  and isPatchGroundedTrigger(anchor)
  and not fixedHasBarrier(anchor)
select anchor, "..."
```

Quality checks:
- Confirm a trigger candidate in patch scope before adding filters.
- Every selected result must be explainable as present before the fix and absent
  or blocked after it.
- For post-read validation patches, anchor stable read/consume touchpoints before
  adding barrier logic.
- Zero hits usually indicate reversed source order, an incorrectly identified
  first/second event, or an unsupported relation.

Validation order: `review_artifact -> codeql_analyze`.
