# V8 all-non-PDS generated-checker evaluation

The complete independent paired screen of the frozen one-per-commit
generate-only KNighter cohort contains 39 execution-valid starting checkers:
14 vulnerable-hit/fixed-noisy and 25 vulnerable-miss cases. No starting
checker is already PDS. **All 39** remain in the common repair population;
neither output arm selects its own denominator. The 39-case design is a
pre-treatment, one-per-commit pilot from 241 generated-only checker records
across 39 commits, not the full upstream population. We retain the 286/45/241
inventory and will report checker-level expansion separately rather than
implying that 39 exhausts the artifacts.

The objective is fixed by the independent starting pair, before model calls:
`precision_refine` for vulnerable-hit/fixed-noisy; `target_hit_recovery` for
vulnerable-miss. Both use the same starting checker, patch, Clang 18 paired
oracle, model, temperature 0.0, eight successful semantic-call ceiling, and
32,768 output-token ceiling. Analyzer-internal evidence is the treatment;
removing only that evidence while preserving the same agent/gates is the
all-39 matched ablation. A missing internal record causes the
native-evidence arm to abstain, while the no-internal ablation still runs on
that same checker with the common source/patch/validation inputs; the
asymmetric applicability is shown explicitly. KNighter's actual false-positive refinement loop is
an additional, direct prior-work comparator **only** for the 14 cases with
fixed-side reports. Its inapplicability to starting target misses is neither
a failure nor imputed as zero; a future end-to-end generation baseline must
be measured on equal inputs before being compared on that stratum.

The primary paired endpoint is PDS: a vulnerable-side alert and fixed-side
silence for the same patch-related target. Secondary observations are
vulnerable-hit recovery, fixed-side alert reduction, and binary pair-level
precision/recall/F1 (one vulnerable positive and one fixed negative per
case). Report full-denominator and starting-status-stratified counts, model
calls/tokens, compile/execution validity, evidence abstentions, and
infrastructure interruptions. A fixed-side decrease without vulnerable hit
is not improvement. Vulnerable recovery with remaining fixed alerts is
partial, not PDS. All 39 case outcomes, including failures and abstentions,
are retained. Repeats are separate runs, never pooled as independent cases.

The evaluation is patch-local Linux CSA. Development metamorphic fixtures
are not held-out generalization evidence. Consistent callback/local renames,
unrelated-statement insertion, branch equivalence, and negative object or
guard controls are reported with their exact denominators and failure modes.
No result is promoted from source-only, model consensus, or a stale upstream
score without independent paired execution.
