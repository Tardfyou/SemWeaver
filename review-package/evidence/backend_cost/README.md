# Backend extension surface and measured CodeQL cost

## What this answers

This audit answers what an integrator must implement, which existing components can be reused, and which machine/model costs were observed for the second backend. It does not estimate person-hours from file size or claim that every new backend is cheap. Historical development was not time-tracked. The CodeQL replay is one development case, not a new effectiveness cohort.

## Integration contract and concrete implementation surface

| Obligation | Existing CodeQL implementation | Reusable part / additional work |
| --- | --- | --- |
| Register detector format and backend operations | `src/core/codeql_analyzer.py`: registered `CodeQLAnalyzer`, `collect_evidence`, `refine`, `validate` | Reuse the analyzer context/result contract; provide backend-specific artifacts and methods. Registration alone is insufficient. |
| Collect and label evidence | `src/evidence/collectors/codeql_flow.py`: `collect`; shared `artifact_extractor.py` | Reuse `EvidenceBundle`/record schema and normalization. Implement native queries or extraction, source anchoring, missing-evidence handling, and provenance checks. A source fallback is not native evidence. |
| Prepare executable detector and dependencies | `src/validation/codeql_support.py`: `ensure_codeql_pack`, database and search-path helpers | Pin CLI, language pack, query dependencies and database revisions. Preserve supplied packs rather than silently changing them. |
| Compile/run and translate results | `src/tools/codeql_analyze.py`, shared `semantic_validator.py` | Map backend failures separately from empty valid results. Decode BQRS tuples instead of CSA diagnostic logs. |
| Enforce paired acceptance | `validate_e2_codeql_pair.py`, `run_e2_codeql_paired_loop.py` | Run exactly the same candidate on both frozen revisions; retain candidate hashes, counts, execution validity and rejected candidates. The generic hit/silence predicate is reused, not the CSA command line. |
| Add a bug mechanism | Existing evidence planner/role vocabulary plus backend-specific predicates and tests | Reuse roles when adequate; add a mapping and safe/unsafe fixtures. Add an extractor only if the required relation is unavailable. No automatic promise of coverage follows from assigning a role. |

The model editor and evidence schema are shared, but several common files contain explicit backend branches. The layer is not a zero-change plug-in system for arbitrary analyzers. The measured E2 loop uses patch/source and analyzer-output feedback; this audit does not turn it into a CodeQL-native-evidence efficacy experiment.

`backend_integration_audit_20260927_v1/RESULT.json` binds an explicit file list to source revision `a5cede46bdd79d91ec5c8874e95c8a0b0206d99b`. Four enumerated CodeQL-specific modules occupy 2,438 physical lines. This includes comments and generation functionality, not just refinement. Shared modules and four external protocol/test files (702 physical lines) are listed separately. These counts describe an implementation footprint, not a marginal patch size, complete dependency closure, person-hours, or a prediction for an unseen backend.

## LLM-assisted planning estimate (not measured)

For a developer familiar with the analyzer, using Codex assistance within the existing framework, **basic backend integration is expected to fit within one afternoon (approximately half a working day)** when the analyzer environment and patch pair are already usable. This is the project owner's qualitative planning estimate, not a timed observation, statistical upper bound, or a claim that the full existing CodeQL implementation was written in that time. It supersedes the earlier conservative multi-day allocation; the whole-file implementation footprint must not be interpreted as code that needs to be rewritten for every integration.

Assumptions: the analyzer is already installed and callable; a supported language, analyzable vulnerable/fixed pair, usable intermediate-output interface, and adequate model access are available. The scope is one worked mechanism and its regression tests, not production hardening or comprehensive vulnerability coverage. Queue/model/tool waiting, unsupported builds, language/runtime changes, and a backend without a usable evidence interface fall outside this allocation.

The basic estimate covers backend registration, command/configuration wiring, result/error decoding, paired validation using the existing interfaces, and a small positive/negative smoke test. Codex can assist with these adapters and tests while the developer checks their behavior.

Implementing a previously unavailable native representation, resolving new interprocedural semantics, or performing broad robustness and production-hardening tests is additional work. No numeric labor estimate is asserted for these backend-dependent extensions. Routine mapping of an already available representation can reuse the existing evidence schema and provenance machinery.

LLM assistance is expected to reduce boilerplate and test-authoring effort; it does not remove the need to check semantics, provenance and error handling. The estimate supports an expectation of low basic adaptation effort under these prerequisites, not an empirically established scalability or labor result.

## Measured resource costs

Actual replay uses **CodeQL 2.26.4** and `codeql/cpp-all` 8.0.3. The previous draft's 2.23.5 was stale and has been corrected. Existing database manifests identify `buildMode: none`.

| Cost component | Observation | Interpretation |
| --- | --- | --- |
| Database preparation | Vulnerable/fixed creation logs span 31/30 seconds using eight threads | Observed successful log intervals, with one-second resolution. Logs say they started late: not full installation or exact end-to-end setup latency. |
| Initial-query paired replay | Median 12.57 s; range 11.96–14.86 s | Three repeated vulnerable+fixed executions, including JSON decoding; all remain 0/0. |
| Refined-query paired replay | Median 36.51 s; range 36.43–36.55 s | Same databases, three repetitions, unchanged model-produced query; all remain 8/0. |
| Peak process RSS | Initial 976 MiB; refined 1,078 MiB | GNU time process maximum, not aggregate concurrent memory. Replay requests two threads and 2,048 MiB. |
| Retained model-run cost | Three original decodes use 1/1/8 model calls and 15,122/14,345/110,282 tokens | Includes the unsuccessful third decode; no new paid model call is needed for this cost audit. |
| Agent-round duration | 87.87/42.74/516.77 s | Sum of retained agent rounds, including their tools but excluding the external paired replay. Not pure API latency or full workflow wall time. |
| Developer labor | Not measured | Do not substitute runtime, commit timestamps or physical lines for engineer-hours. |

The resource probe retains existing database and library caches and creates a fresh query directory per repetition. Query order alternates. It is not a cold installation benchmark, a CSA-versus-CodeQL speed comparison, a whole-project scan claim, or a throughput extrapolation. The refined query is more expensive than the initial target-missing query, so the result does not support a speedup claim.

## Evidence and verification

- `backend_cost_20260927_v1/`: frozen query/pack/database identities, CLI version, 12 successful side executions, GNU time records, decoded rows and summary.
- `backend_cost_summary_20260927_v1/`: original database-creation logs and hash-bound joins to all three retained model runs.
- `backend_integration_audit_20260927_v1/`: enumerated implementation files and their source hashes.
- `measure_backend_cost.py`, `audit_backend_integration.py`, `summarize_backend_cost.py`: executable measurement/audit drivers; none edits checker/query semantics or calls a model.
- `test_backend_cost.py` and `test_e2_codeql_paired_loop.py`: six passing local protocol tests, including failure/timeout distinction and paired acceptance. The five CodeQL-pack/provenance tests also pass in the project's dependency-complete environment (`source-tests-correct-env.xml`). Earlier lightweight-host collection errors are retained as environment diagnostics, not counted as passed tests or experimental results.

The release builder includes these records only as a separate cost/integration appendix. They do not change the frozen 39-case counts or the three E2 effectiveness decodes. Publication remains deferred until the outstanding model experiment and full-package checks finish.
