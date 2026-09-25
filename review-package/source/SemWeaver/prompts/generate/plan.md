You are in the planning phase of detector generation.

{{TASK_PROMPT}}

Patch:
```diff
{{PATCH_TEXT}}
```

`analyze_patch` output:
```text
{{ANALYSIS_TEXT}}
```

Planning requirements:
- Choose the checker/query name.
- Classify the patch shape: added guard, deleted dangerous operation, safe API
  replacement, state/ownership change, range/length change, or other.
- Define the patch-local trigger: changed file/function/expression,
  variables/fields/APIs, and nearby release/use/reassign/guard events.
- The knowledge query must include both vulnerability topic and patch shape.
- `pattern_description` must state the minimum first-draft trigger. Add complex
  modeling only when that trigger exists but is insufficient.

Return exactly one JSON object:
{
  "summary": "One-sentence plan",
  "checker_name": "Stable class or query name without extension",
  "knowledge_query": "Query for search_knowledge",
  "vulnerability_type": "buffer_overflow / use_after_free / null_dereference / unknown / ...",
  "query_description": "Short query description",
  "pattern_description": "Patch shape, patch-local trigger, and any boundary on complex modeling"
}
