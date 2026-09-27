You are in the first-draft phase of detector generation.

{{TASK_PROMPT}}

Target file name: {{CHECKER_NAME}}
Target path: {{ARTIFACT_PATH}}
RAG match: {{RAG_MATCH}}

Patch:
```diff
{{PATCH_TEXT}}
```

`analyze_patch` output:
```text
{{ANALYSIS_TEXT}}
```

`search_knowledge` output:
```text
{{KNOWLEDGE_TEXT}}
```

`rag_check` decision:
```text
{{RAG_CHECK_RESULT}}
```

Reference skeleton (first turn only):
{{REFERENCE_SKELETON}}

Draft requirements:
- When RAG matches, reuse stable APIs and organization, but remove triggers or
  guards that do not match this patch shape.
- Otherwise, write a fresh draft and use the skeleton only for file structure.
- Implement the minimum patch-local trigger before generalizing.
- For a deleted dangerous operation, trigger on that operation. Inspect same-
  function events on the same resource before considering interprocedural facts.
- For an added guard/barrier, first match the original source/sink; add a real
  `not hasGuard` relation only when supported.
- For a safe API replacement, match the old API and the same argument roles.
- Do not require a callee to receive a resource unless the patch or call
  arguments show that relation.
- Do not make multi-level dominance, global flow, or call chains first-draft
  requirements.
- Output neither explanation, diff, nor a multi-file plan.

Return one parseable JSON object only. `content` must be one complete source or
query string. Even failure must return valid JSON.

{
  "summary": "One-sentence draft strategy",
  "checker_name": "Optional replacement name",
  "content": "Complete detector/query source"
}
