# Evidence scope and interpretation

The package distinguishes measured outcomes, implementation capabilities, estimates and untested properties.

## Measured

- All 39 frozen, automatically generated starting CSA checkers are independently executed on their designated vulnerable/fixed pair. Fifteen warn on both revisions and 24 are silent on the vulnerable revision. Warning positivity is not a confirmed target-detection label.
- The direct false-positive comparison covers all 15 fixed-noisy cases with the same model and starting reports under case-specific KNighter-derived call ceilings. The broader 39-case comparison additionally covers SemWeaver recovery attempts; KNighter's original no-report branch does not perform those edits.
- Warning-positive/negative patch versions define the secondary precision/recall/F1 values. Warnings in fixed-side patch-related objects are a task-specific noise measure, not an independently adjudicated population of false bug reports.
- All native records have a backend-output/source-scope binding. Their presence does not prove path feasibility, ownership or semantic correctness of a generated edit. The full-cohort native/no-internal outcome ties; no universal causal advantage is asserted.
- Repeated decodes use the same retained 12 subjects and do not increase the number of independent checkers. Full-model and repeated-subset budgets are recorded separately.

## Development diagnostics

Warning positivity is not sufficient evidence of correct target localization. G08 is a concrete counterexample: its remaining warning concerns an allocation-failure path, whereas the patch repairs a later registration-failure leak. Its 2/2 to 1/1 change is not a demonstrated target-preserving improvement. See `TARGET_ALIGNMENT_NOTES.md` for static source/diagnostic checks, including the five newly warning-positive GPT cases; these checks are not an exhaustive per-warning ground-truth dataset.

The motivating example's final checker is model-produced and independently replayed on Linux. Its 114 successful diagnostic executions include parameterized variants of 20 control-flow forms; they are not 114 independent vulnerabilities. Fixtures and diagnostic feedback are developer-authored, earlier failures are retained, and the resulting development continuation is not substituted into main-cohort scores.

The separate CodeQL result concerns one archived generated query and three automatic decodes. It demonstrates patch-local execution/refinement on a second format, not broad cross-backend or cross-generator transfer. It is not a KNighter/CodeQL comparison.

## Costs and untested properties

CodeQL setup log intervals, repeated query wall times, process RSS and original model-call/token usage are measured or directly reconstructed from retained logs as labelled. The within-one-afternoon Codex-assisted basic-integration expectation is a conditional planning estimate, not a person-hour experiment. Full installation, production hardening and new semantic analyses are outside that estimate.

The study does not establish whole-kernel throughput, general vulnerability-family recall, stability across unseen projects, optimal evidence routing, or effectiveness on manually authored analyzer checkers. Existing compatible checkers can use the interface, but technical compatibility is not empirical validation of that population.

## Failure handling

Infrastructure and uncaught SDK/agent errors are not scored as detector failures. Candidate compilation/loading failures and execution-invalid attempts remain explicit; strict portfolio scoring falls back to the independently validated starting checker when no candidate is adopted. A failed scan never becomes a valid zero-warning result. The correction map and replaced-cell queues retain the audit trail without treating obsolete flags as current evidence.
