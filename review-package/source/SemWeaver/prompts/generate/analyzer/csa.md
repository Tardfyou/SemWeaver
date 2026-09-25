CSA objective: generate a Clang 18 plugin checker, compile it as `.so`, and
validate it under `custom.<checker_name>`.

Required exports:
```cpp
extern "C" void clang_registerCheckers(CheckerRegistry &Registry) {
  Registry.addChecker<YourChecker>("custom.YourChecker", "Description", "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
```

API constraints:
- Registration, class, primary file class, and `checker_name` must agree.
- Place `REGISTER_SET_WITH_PROGRAMSTATE` / `REGISTER_MAP_WITH_PROGRAMSTATE`
  outside the class.
- Do not use `Stmt::getParent()`, `Stmt::getParentStmt()`, or
  `Expr::getParent()`; avoid parent lookup unless necessary.
- Do not call `MemRegion::getValueType()`; cast to `TypedValueRegion` first.
- Use `StringRef::starts_with()`, `ends_with()`, `contains()`, and
  `contains_insensitive()`.
- Null-check every `Expr*`, `Stmt*`, `MemRegion*`, `IdentifierInfo*`, and
  `ValueDecl*` before use.

Clang 18 compatibility:
- Include the concrete AST, source-location, bug reporter, checker, call-event,
  checker-context, region/SVal, and checker-registry headers actually used.
- Do not include `clang/StaticAnalyzer/Frontend/AnalysisManager.h` or
  `clang/StaticAnalyzer/Core/PathDiagnosticLocation.h`.
- `check::ASTCodeBody` has signature
  `void checkASTCodeBody(const Decl *D, AnalysisManager &, BugReporter &BR) const`.
- Treat `AnalysisManager` as a placeholder in `checkASTCodeBody`.
- `BugReporter::getSourceManager()` returns `const SourceManager &`.
- Treat read-only `AnalysisManager`, `ASTContext`, and `SourceManager` as
  `const &`. Do not pass `BT->getName()` to `EmitBasicReport`.

Reporting:
- In path-sensitive callbacks, use `PathSensitiveBugReport` only with a node
  from the current `CheckerContext`.
- `check::ASTCodeBody`/`RecursiveASTVisitor` has no real `ExplodedNode`; use
  `BR.EmitBasicReport(...)` or `BasicBugReport`.
- Return when `C.generateNonFatalErrorNode()` yields null.
- A stable `EmitBasicReport` form uses the current `FunctionDecl`, checker,
  title/category/description, a `PathDiagnosticLocation`, and source range.

Modeling priorities:
1. Establish the minimum patch-grounded trigger using the changed expression and
   nearby resource/state relations.
2. A first draft may use patch scope to locate the mechanism, but exact function,
   label, variable, literal, or adjacency matches cannot be the detector's sole
   semantic condition.
3. For deleted calls/releases/writes, start with the removed operation.
4. For added guards/barriers, first match the original source/sink.
5. For API replacement, match the old API and the same argument roles.
6. Use interprocedural or ProgramState modeling only when local facts are
   insufficient.
7. When a parse/read/write touchpoint is the stable anchor, use it directly
   rather than inventing a missing-guard shape.
8. Cover `BinaryOperator` assignments, `VarDecl` initializers, and compound
   assignments including `|=` and `+=`.

Deleted release/destroy patches:
- In the changed function, inspect earlier/later release, use, reassign, and null
  events for the same resource before moving to callers.
- Do not require a callee to receive the resource unless call arguments prove it.
- Strip casts, parentheses, and address-of operators from release arguments;
  compare `VarDecl`/`FieldDecl` identity.
- If the removed release is the second release, search earlier, not later, for
  the first release.

Implementation strategy:
- `check::ASTCodeBody` is suitable for recursive structural relations, but an
  exact enclosing-function filter is only a localization aid and must not decide
  the warning.
- Use `PreCall`, `PostCall`, `Bind`, `BranchCondition`, and ProgramState when the
  mechanism requires actual path or ownership state.
- Prefer a direct callback at a stable API/state touchpoint when AST traversal
  would make the trigger empty.
- Recursively traverse nested control flow and expression trees, stripping casts
  and parentheses and visiting references, members, calls, and subscripts.
- Do not conjoin source, propagation, guard, and sink into one fragile condition
  before a patch-grounded trigger exists.

Quality target:
- Explain why vulnerable behavior near the patch warns.
- Do not trade the initial hit for speculative generalization, but do not encode
  an exact patch fingerprint as the final rule.
- Compilation/LSP repair may fix only APIs or report construction, never remove
  the core detection relation.
