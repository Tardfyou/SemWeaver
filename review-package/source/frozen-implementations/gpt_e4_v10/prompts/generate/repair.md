You are in generation repair. Address only the latest failure.

{{TASK_PROMPT}}

Target name: {{CHECKER_NAME}}
Artifact path: {{ARTIFACT_PATH}}

Current artifact:
```text
{{ARTIFACT_TEXT}}
```

Failed tool: {{LATEST_FAILURE_TITLE}}

Failure details:
```text
{{LATEST_FAILURE_TEXT}}
```

Repair rules:
1. Fix only the latest syntax, API, type, scope, or local-structure failure.
2. Order: API/member name -> argument types/count -> scope -> include/import.
3. Add a header only after confirming it exists.
4. Use exact local replacements; do not rewrite the file.
5. Never delete the existing vulnerability semantics to fix compilation.
6. For `semantic_no_hits`, `executed_no_hit`, or target miss, minimally correct
   a reversed or overly narrow trigger using patch-local facts.
7. For a deleted dangerous operation, check whether it was incorrectly treated
   as the first event or constrained by an unsupported callee/resource relation.

CSA rules:
- Target Clang 18. Do not use `Stmt::getParent()`, `Stmt::getParentStmt()`, or
  `Expr::getParent()`; parent lookup uses ParentMap/ASTContext only if necessary.
- Use `StringRef::starts_with()`.
- Use `PathSensitiveBugReport` only with a real node from the current
  `CheckerContext`.
- `check::ASTCodeBody`/`RecursiveASTVisitor` has no real `ExplodedNode`; use
  `BR.EmitBasicReport(...)` or `BasicBugReport`.
- Preserve semantics when locally replacing invalid report construction.
- Keep `clang_registerCheckers` and `clang_analyzerAPIVersionString` valid.

CodeQL rules:
- Declare quantified variables in the same `exists()` and never reference a
  sibling quantifier's local.
- Check parentheses and quantifier closure.
- After two failures on an exact API/type/member name, stop guessing and consult
  a validated repository query or minimal probe.
- If `old_snippet` does not match, reread the current artifact and submit a
  smaller unique edit.

Output `finish` when no repair is needed; otherwise output `apply_patch`.
Return exactly one JSON object:
{
  "action": "apply_patch" | "finish",
  "summary": "One short sentence",
  "edits": [
    {
      "old_snippet": "Unique current snippet",
      "new_snippet": "Replacement"
    }
  ]
}
