Decide whether the retrieved knowledge matches the vulnerability evidenced by
this patch.

Patch:
```diff
{{PATCH_TEXT}}
```

`analyze_patch` output:
```text
{{ANALYSIS_TEXT}}
```

Retrieved knowledge:
```text
{{KNOWLEDGE_TEXT}}
```

Criteria:
1. The vulnerability type matches.
2. The patch shape matches; for example, an added guard and a deleted release
   are not the same skeleton.
3. Trigger and fixed-side silence mechanisms match, not merely the CWE label.
4. APIs, AST types, and callback/query structures apply to this patch.

Accept only when the mechanism and patch shape match and the skeleton helps form
a patch-local trigger without imposing unseen interprocedural or guard
conditions. Reject broad family matches that would filter the trigger to zero.

Return exactly one JSON object:
{
  "match": true,
  "reason": "Why the result matches or does not match",
  "reuse_strategy": "How to reuse it, or state that a fresh draft is required"
}
