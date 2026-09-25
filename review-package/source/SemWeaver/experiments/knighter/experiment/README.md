# Knighter Experiment Drivers

These scripts operate on Knighter case directories and baseline/refined checker
outputs. They are source-only and expect their inputs under
`artifacts/experiments/knighter/experiment`.

Included drivers:

- `prepare_ablation_cases.py`: creates controlled evidence-ablation inputs.
- `collect_ablation_result.py`: summarizes strict ablation scan logs.
- `collect_model_result.py`: summarizes model-robustness scan logs.
- `run_refined_fixed_fullscan.py`: runs selected refined checkers against fixed kernel revisions.
- `run_matched_knighter.py`: executes the upstream refinement loop on one frozen
  subject with an environment-provided OpenAI-compatible model and packaged
  Clang 18 paths.
- `resume_matched_validation.py`: resumes only deterministic validation for an
  already-frozen LLM candidate after a documented infrastructure/adapter
  failure; it makes zero model calls and binds the candidate and responses by
  SHA-256.
- `run_matched_knighter_batch.py`: freezes report/checker/patch hashes and
  executes the single-subject runner sequentially over a cohort.
- `summarize_matched_knighter_batch.py`: independently verifies result bindings,
  classifies failures from raw run logs, and reports exact binomial intervals.
- `run_semweaver_treatment_case.py`: runs one provenance-gated treatment
  attempt. A later attempt may consume either a hash-bound paired-validation
  result or a failed run manifest plus its exact candidate; the driver derives
  typed feedback from machine-readable artifacts and rejects mismatched hashes.
  New runs persist every exact system/user prompt and raw model response in
  `llm_exchanges.jsonl`; the run manifest records the log's SHA-256.
- `collect_frozen_csa_evidence.py`: removes legacy evidence/feedback from a
  hash-bound case plan and replays the current Clang collector without a model
  call. With `--prepare-vulnerable`, it builds the frozen patch objects and
  regenerates the compilation database before extraction. A case with zero
  strict analyzer-internal records exits as abstained.
- `collect_frozen_csa_evidence_batch.py`: freezes all case input hashes from a
  cohort manifest, executes the single-case replay sequentially, and persists
  eligible, abstained, and execution-error rows without dropping failures.
- `audit_csa_evidence_binding.py`: blocks treatment unless every internal record
  is bound to a patch file/function and its persisted Clang interface, schema,
  raw artifact, raw hash, and command hash verify.
- `run_semweaver_treatment_batch.py`: freezes the eligible evidence cohort and
  allocates each subject the model-call count observed for its matched KNighter
  counterpart (19 calls total), then classifies only external paired validation.
- `run_semweaver_full_treatment_batch.py`: runs the separate RQ1 full-budget
  condition. It divides each subject's call cap into bounded attempts and uses
  only hash-bound compile/paired failures for continuation; the first PDS
  candidate is frozen and stops further model calls.
- `summarize_deploy_or_fallback_alerts.py`: recomputes aggregate fixed-side
  alert burden under a PDS-only adoption policy; rejected runs fall back to the
  original checker rather than being counted as deployed regressions.

The scripts assume the external Linux/LLVM environment is available under
`artifacts/external/` unless overridden on the command line.

## Packaged Clang 18 smoke

`observed_packaged_clang_smoke.json` records a completed environment check using
the unmodified case-07 baseline checker, a blob-filtered Linux repository with
the exact commit and parent, and a packaged Clang 18 plugin build. The replay
reproduced the legacy strict object counts exactly: 10 vulnerable-side and 9
fixed-side scan-build reports.

The smoke alone validates the execution environment only; it is not a matched
refinement result. The completed cohort condition is documented below.

## Observed matched-loop pilot

`observed_matched_knighter_case07.json` records a completed, execution-valid
single-case run of the actual Knighter refinement path. The model correctly
triaged one fixed-side report as a false positive and produced a compilable
candidate. The candidate removed that warning but also lost the vulnerable-side
hit, yielding paired TP/TN `0/1`; it is therefore a validity-loss failure, not
an accepted refinement.

Two preceding attempts are explicitly invalidated: one lacked `clang` on the
subprocess path, and one exposed the upstream basename-to-object binding bug.
The latter candidate was frozen before the fail-closed adapter correction and
then deterministically revalidated without another model call. Raw prompts,
responses, candidates, logs, and invalidation sidecars remain in the external
artifact tree. This one-case pilot validates the matched-loop harness and a
failure category only; it is not a 12-case cohort result.

## Completed model-matched cohort

`observed_matched_knighter_cohort.json` records the completed 12-subject
fixed-noisy condition. All 12 executions were valid. The actual upstream loop
accepted one refinement (1/12, 8.3%; exact 95% CI 0.2%--38.5%). Five subjects
failed because the model classified the fixed-side report as a true positive,
four produced a non-compiling candidate, and two retained fixed-side noise. The
condition used 19 model calls and 215,997 observed tokens.

The sole PDS success was then frozen and evaluated separately: deterministic
artifact review returned no findings or warnings, and a post-freeze suite passed
6/6 positive and 3/3 negative fixtures, including local renaming, independent
statement insertion, explicit-null rewriting, boolean composition, wrapper
extraction, a proper status guard, an alternate API, and no-pointer-branch
near-misses. These data establish the model-matched Knighter condition only;
they do not supply a repeated-decode comparison.

## Completed call-matched SemWeaver condition

The external `semweaver_treatment_matched_v3` batch assigns every subject the
one- or two-call cap observed for its Knighter counterpart. It completed all 12
subjects with 19 calls and 287,332 observed tokens, producing one PDS candidate
on case 19. The paired PDS comparison is therefore 1/12 versus 1/12, with one
discordant subject in each direction (exact McNemar p=1.0).

`observed_semweaver_matched_metamorphic_v3.json` records a deterministic
post-freeze evaluation of that sole candidate. The candidate had been frozen
before either suite was authored. It passed 4/6 positives and 3/3 negatives in
the first suite, then 0/6 positives and 3/3 negatives in the fresh suite. It is
therefore patch-bound and does not satisfy RRS. The result JSON binds the
candidate, batch inputs, both suite manifests, and both raw result files by
SHA-256. The Knighter and SemWeaver PDS candidates occur on different patches
with different mechanism-specific suites, so these suite outcomes are not used
as a method-level generalization comparison.

## SemWeaver case-05 development closure

`observed_semweaver_case05_development.json` records the development-only
feedback lineage used to harden the CSA treatment path. The final automatically
edited candidate compiles, achieves paired counts 9/0 (PDS), and passes the
revised development suite at 6/6 positive and 3/3 negative variants. Its bundle
contains 13 records: one strictly analyzer-internal CSA CFG record and twelve
source-derived records.

This is not a primary treatment or confirmatory RRS result. The subject and
suite diagnostics informed prompt and deterministic-gate changes, and the effective lineage consumed 20 model calls
and 272,670 observed tokens, exceeding the completed matched baseline's 19
calls and 215,997 tokens. It establishes that the feedback/gating design can
reach an RRS candidate and supplies regression cases; comparative claims require
a fresh frozen cohort run with no intervening adaptation.

## Completed full-budget development cohort

`observed_semweaver_full_budget_v3.json` binds the completed 12-subject,
eight-call-cap condition. It produced four PDS candidates (33.3%, exact 95%
CI 9.9%--65.1%) using 86 model calls and 1,252,234 observed tokens. The strict
evidence replay contained 37/70 analyzer-internal records, all 37 bound to a
patch file/function and verified raw Clang artifact; no manual or unknown
records entered the bundles.

PDS did not survive the stronger outcome contract. Every accepted candidate
failed at least one post-freeze transformation suite, so RRS is 0/12 (exact
95% CI 0%--26.5%). The four failures include wrapper extraction, ordinary
renaming/direct-initializer changes, an inverted failure condition, a commuted
guard, and a mechanism-breaking larger-capacity near miss. This completed run
is therefore a development baseline and a source of regression tests, not an
effectiveness result that can support the paper's primary claim.

Under the explicit deploy-or-fallback policy recorded in
`observed_deploy_or_fallback_alert_burden.json`, the 12 starting checkers emit
37 fixed-side alerts. The two 19-call conditions each accept one checker that
removes one alert, retaining 36/37. The 86-call SemWeaver condition accepts four
PDS checkers whose starting fixed sides account for nine alerts, retaining
28/37 (24.3% fewer than the starting portfolio and 22.2% fewer retained alerts
than the model-matched Knighter portfolio). This is a useful budget-sensitivity
result, not a matched-budget method advantage.

`observed_semweaver_targeted_postgate_v2.json` records the first targeted
regression after adding vulnerable-revision source snapshots, compile-after-edit,
and three metamorphic structural gates. All three runs produced locally
compilable candidates, but only case 19 retained PDS. It passed the suite used
during development, then failed a newly frozen confirmation suite (0/6
positives and 2/3 negatives). This demonstrates why reusing a development
transformation suite as confirmation would materially overstate robustness.
