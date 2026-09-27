# V8 generate-only matched-refinement protocol

This protocol was fixed before running either refinement method on the new
generate-only cohort. It supplements, and does not overwrite, the original
12-case study. All new observations are kept under `generate_only_*` artifact
directories with source hashes and case-level statuses.

## Source population and screening

The upstream KNighter `knighter_gen_single_checkers.csv` contains 286 generated
checker records for 60 commits. The 45 old-score-valid records all appear in
the upstream refinement-status table. The other 241 generated checker IDs
across 39 commits lack a corresponding upstream refinement record. This is a
historical artifact state, not proof that upstream would accept every checker
for refinement. We select one generated-only checker per commit by the frozen
upstream pre-treatment ordering (higher old TP, then higher old TN, then lower
checker index). We use the generated record's nonempty repaired source, whose
hash and `source_role` are retained. The old `score_valid=False` field is not
used as a new paired-execution label.

Every selected checker is independently compiled and run on the patch's
vulnerable parent and fixed commit before either method is applied. The
screened denominator is 39; the refinement-eligible subset is **all** cases
with a valid vulnerable-side hit and at least one fixed-side alert. Already-PDS,
vulnerable-side misses, compile failures, and execution-invalid cases remain
in the screen ledger and cannot be silently removed or called failures of
either refiner. The screen result and report HTML bytes are frozen before
refinement. We do not add or remove eligible cases based on either refiner's
result.

## Matched methods and outcomes

Both methods start from the same checker source and patch. KNighter uses its
actual refinement prompt/loop with the original fixed-side reports, not a
SemWeaver ablation. The packaged adapter corrects one upstream argument bug:
`check_report` receives the frozen patch in its patch slot rather than a
second copy of the bug-pattern text. This is disclosed as a fair-input
correction, not claimed to be a byte-identical upstream execution. SemWeaver
uses the frozen patch-derived plan and separate
analyzer-native evidence bundle. Both use `glm-5.3-flash` through the same
BigModel OpenAI-compatible Chat Completions endpoint and private credential.
An evidence-gate abstention is a valid SemWeaver non-refinement in the full
eligible denominator, not a reason to drop the case; an evidence collector
crash is an infrastructure failure and remains unscored. We additionally
report a descriptive evidence-available stratum, with its denominator shown.
Transport errors and 429s are retryable infrastructure events, never PDS or
fixed-side outcomes. Per-case model calls, token counts, compile attempts,
elapsed time, input hashes, and output hashes are retained. The method-specific
prompts differ by design; the model, source inputs, target pair, and execution
oracle are matched. We report both configured and observed request/token
budgets; any asymmetry is explicit rather than described as strict parity.
The primary GLM-Flash condition caps each arm at eight successful semantic
model calls per case and 32,768 output tokens per call. Transport retries do
not consume a semantic call. KNighter may use up to two native refinement
attempts and five distinct fixed-side reports; SemWeaver may use up to eight
model turns, with every candidate checked by the independent paired oracle.
Both arms request temperature 0.0 on the GLM Chat API.
Reached call caps are method failures, not infrastructure errors. The same
caps apply to the no-internal-evidence condition. The configured cap is an
upper bound, not an assertion that both methods actually used equal tokens.

Every candidate is then validated independently with the same packaged Clang
18 CSA implementation against the same vulnerable and fixed revisions. The
behavioral endpoint is PDS (at least one vulnerable-side alert and zero
fixed-side alerts). Structural quality review is a separate adoptability
dimension, not a substitute for executing the checker. Fixed-side alert counts
are reported with an explicit denominator; this is patch-local precision, not
general vulnerability-family recall. We also report invalid execution,
vulnerable-hit loss, remaining fixed noise, and no-change outcomes separately.
For the requested F1 view, each eligible patch pair contributes one positive
(vulnerable revision) and one negative (fixed revision): a vulnerable hit is
one TP, a vulnerable miss one FN, any fixed-side hit one FP, and a fixed-side
silent run one TN. We compute micro precision, recall, and F1 from these binary
case-level counts, never from raw alert multiplicities; the raw fixed-side
alert reduction is a separate measure. With only one positive and one negative
per patch, F1 is supplementary to the stricter per-pair PDS endpoint.
If either arm has an infrastructure failure, its outcome is `unscored`; no
zero-filling, pairwise imputation, or selective case replacement is allowed.

## Ablation and robustness

The no-internal-evidence ablation removes analyzer-internal records from the
same SemWeaver agent while leaving its compiler and paired oracle unchanged.
It is not labeled a KNighter baseline. Model robustness reruns the frozen
methods and cohort, where feasible, with `glm-5.3-flash`, `glm-5.3`, and
`gpt-6-luna` at high reasoning effort, reported as separate conditions rather
than pooled results. Replicates use distinct output directories and report
per-case success frequency. The exact available budget and any incomplete
condition must be shown; a single run is never described as variance evidence.

## Inference boundary

The cohort is Linux patch-local C/CSA. Its 39 records are one-per-commit
generated-only selections, not a random draw from all security patches or all
241 generated-only records. The paired oracle does not prove semantic absence
of all false positives or cross-project transfer. Any illustrative example
chosen after results is labeled illustrative, and separately constructed
metamorphic fixtures cannot be treated as held-out unless frozen before the
candidate is inspected.
