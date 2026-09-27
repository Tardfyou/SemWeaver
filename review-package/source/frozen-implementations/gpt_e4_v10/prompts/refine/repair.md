You are in the refinement repair phase. Fix only the latest validation failure;
do not reopen patch-mechanism analysis.

{{TASK_PROMPT}}

Artifact path: {{ARTIFACT_PATH}}

Current artifact:
```text
{{ARTIFACT_TEXT}}
```

Latest failed tool: {{LATEST_FAILURE_TITLE}}
Focused lines: {{LATEST_FAILURE_LINES}}

Failure details:
```text
{{LATEST_FAILURE_TEXT}}
```

Repair rules:
1. Fix only the latest failure, prioritizing syntax, API, type, quantifier,
   scope, or local-structure errors.
2. This round has left decide. Whether the failure came from LSP, review,
   compilation, CSA functional validation, or CodeQL analysis, do not redo the
   mechanism design.
3. Submit only unique exact snippet replacements. Output `finish` if no change is
   needed.
4. Never fix a failure by deleting the semantic relation, changing an
   evidence-origin label, or making an exact line/label/name match the sole
   warning predicate. The harness already limits the patch-related objects;
   do not bypass a review finding by adding an exact AST-body function filter.
5. After repair, the same gates rerun. A new decide phase begins only after the
   entire round passes.

CSA rules:
- Do not use nonexistent Clang 18 APIs.
- Do not use `Stmt::getParent()`, `Stmt::getParentStmt()`, or
  `Expr::getParent()`.
- `StringRef::starts_with()` is the correct API.
- `State->get<MapName>(key)` has pointer semantics, not `std::optional`.
- The Clang~18 assumption callback is `eval::Assume` with
  `ProgramStateRef evalAssume(...) const`, not `check::Assume`/`checkAssume`.
- `checkBranchCondition` receives `const Stmt *`: first `dyn_cast<Expr>` and
  only then call `IgnoreParenImpCasts()`. Do not call `State->getSymVal`; use
  `State->getStateManager().getConstraintManager().getSymVal(State, Symbol)`.
- For `&output`, call `getMemRegionFromExpr` on the original address expression,
  not on the `getSubExpr()` value; the latter can be regionless in CSA.
- Preserve `VarDecl` initializer consumers in integer-width flow, unwrap
  negated registration calls before branch selection, and require downstream
  target-capacity binding for off-by-one repairs.
- Do not repair a status barrier by setting a checked bit on every branch.
  Preserve the return `SymbolRef` and discharge the pending output only on the
  `evalAssume` state where `ConstraintManager::getSymVal` proves status zero.
- For a review finding, remove fake helpers or bind them to real
  guard/region/state semantics. Use function context to identify the relevant
  relation; keep the checker free of exact patch-function name filters.
- Do not call nonexistent `createBegin` APIs. If using
  `ASTContext::getParents`, include `clang/AST/ParentMapContext.h` for the
  complete return type. Prefer inspecting existing integer expressions to
  constructing `IntegerLiteral` nodes.
- Define or forward-declare every called helper. Keep
  `REGISTER_*_WITH_PROGRAMSTATE` at namespace scope, not inside the anonymous
  checker namespace. Use `dyn_cast<FunctionDecl>` before assigning a generic
  `Decl *` to a `FunctionDecl *` variable.

CodeQL rules:
- Declare quantified variables in the same `exists()` parameter list.
- Do not reference variables outside their quantifier.
- Fix scope, parentheses, API names, and types first.
- Do not invent `IntegerLiteral` or `IntLiteral`; reuse validated repository
  patterns or a more general expression/constant predicate.

Return exactly one JSON object with no Markdown or commentary:
{
  "action": "apply_patch" | "finish",
  "summary": "One short sentence describing this local repair",
  "edits": [
    {
      "old_snippet": "A unique snippet in the current artifact",
      "new_snippet": "The replacement"
    }
  ]
}
