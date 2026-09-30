# Final meaning-preserving prose edits

## Current pass: empirical contribution and mixed outcomes

The author's current request is to foreground the contribution of explaining
the benefits and costs of analyzer evidence, with stronger presentation as an
editorial objective. This pass edits the active manuscript in place, as requested.
It makes no measured claim about reviewer scores or acceptance.

Reading covered all manuscript sections, definitions and retention equations,
tables and figure captions, the supplied bibliography, main and auxiliary
results, and `COMPARATIVE_MECHANISM_FINDINGS.md` / the associated case ledger.
Existing numerical and provenance audits were consulted. No new experiment,
literature search, reviewer query or simulated scoring panel was run.

| Passage | Changed cue and rationale | Evidence anchor and preserved meaning |
|---|---|---|
| Abstract opening and final sentences | S1/S3: foreground the implemented workflow together with its empirical characterization; retain the result order and qualifications. R1/R2 motivate contribution stance and R3 motivates abstract emphasis, without establishing a score gain for these edits. | `FINAL39_SUMMARY.json` and `AUXILIARY_SUMMARY.json`; all 39 subjects, 15/24 starting split, actual KNighter loop, 23/15 coverage, F1 .575/.448, 44/69 reports, six/five comparisons, noise increase and sampling dependence remain explicit. |
| Introduction and contribution 3 | S1/S2: state the existing complete comparison and code analysis as findings about warning recovery and report burden. | Same main/auxiliary summaries and `COMPARATIVE_CASE_CODE_LEDGER_V2.json`; no new claim of optimal routing, native necessity, general superiority or a new benchmark population. |
| RQ1 / RQ3 / qualitative findings | S2: express the same metrics as different coverage/noise portfolios and connect observed outcomes to retained code edits. | Main Table 3 and complete non-tied case ledger. Near-equal F1 is not statistical equivalence; G12 remains noisy and G24 target preservation remains unverified. All table cells and repeated outcomes are unchanged. |
| Discussion | Author-requested synthesis, distinguished from a near-equivalent wording substitution: organize existing G19/G12/G24 observations around edit behavior and practical interpretation. S5 keeps the empirical boundaries adjacent. | Missing cleanup-wrapper recognition, broadened matching and ambiguous report reduction are recorded code observations. Inspecting the changed predicate alongside both counts is an implication, not a tested prediction rule for when evidence helps. |
| Conclusion | S1/S5: close on both the workflow and its empirical characterization. | Original comparisons, one-subject concentration, overall noise increase, tied F1, repeated variability, single-case CodeQL and target/generalization limits remain. |

S6 editorial review checks claims, comparisons, causal status, denominators,
measurements and substantive limits against the pre-edit prose and those anchors.
No statistical or causal finding is created through phrasing. The full Threats
to Validity section, selection/reuse disclosures, budgets, models, methods,
equations, citation keys, bibliography, all data tables and author figures are
unchanged. The earlier 114-fixture illustration remains distinct from the
74/114 main checker, and single-decode E4 remains configuration sensitivity.
The known caveats are not treated as resolved by the new presentation.

## Earlier prose checkpoint

This pass followed substantive revision and the F258 visual checks. Reading
covered the abstract, introduction/contributions, methods/algorithm, study
design, results/tables/captions, discussion, validity, related work, conclusion
and their main/auxiliary evidence. Three small rhetorical edits were selected,
plus one grammar correction. No reviewer was queried for a score, no hidden
directive was added, and no review/acceptance improvement is asserted.

| Passage | Changed cue | Scientific meaning held fixed |
|---|---|---|
| Abstract opening | Contribution stance: present the implemented workflow, rather than describe studying refinement. | Automated post-synthesis scope, paired execution and the three evidence origins remain unchanged; supported by the v43 implementation and method algorithm. No new priority or efficacy claim. |
| Abstract main comparison | Evidence framing: put warning-positive versions23 versus15 before F1 .575 versus.448. | Same39 subjects, GPT model and actual KNighter workflow. The noise increase, mixed ablation, repeated-sampling dependence and target/generalization limits remain in the same paragraph; source `FINAL39_SUMMARY.json`. |
| Conclusion stratum | Evidence framing:69 versus44 becomes a within-stratum reduction from69 to44 with warning retention. | Comparator is KNighter; the metric is reports, not adjudicated false positives. The complete15-case denominator, one-subject concentration and whole-cohort noise increase remain adjacent. |

The ordinary grammar edit changes `a ambiguity` to `an ambiguity`. A separate
Chinese note is a private user-facing handoff and is not intended for the
English anonymous replication package.

Editorial equivalence checks preserved numbers, units, comparators, scope,
causal status, uncertainty and adverse findings. Near-equal F1 is not recast as
statistical equivalence. This is an editorial check, not measured human-review
agreement. Selection/reuse, G24's unadjudicated targets, G12 noise, E3's lack of
all-three comparative benefits, single-decode E4, one-case CodeQL and the
fixture-informed14-response illustration remain explicit. Compilation, final
rendering, anonymity and release verification are separate gates.
