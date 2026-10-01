# Methods and interpretation

## E4 repeated model sensitivity

The twelve inputs are the same fixed subset used for E3, not twelve new bugs per
repeat. The first 36 subject-model trials reuse existing E4 outputs; two new
decodes add 72 trials, for 108 recorded trials on twelve underlying subjects.
Each decode has a two-reply ceiling and the recorded 65,536-output-token ceiling.
The experiment is native-only: it cannot estimate the evidence ablation within
each model. Invalid proposals and failed candidate validation consume budget;
only normally paired-executed retained checkers contribute warning metrics.
G25/GLM-5.3/repeat-2 retained the original healthy checker after a generated
candidate's reproducible scanner crash. Its original failed outputs and the
deterministic no-network recheck are retained, not counted as zero-warning
candidate evidence.

| Editor | F1 by decode | Mean (sample SD) | PDS | Fixed reports | Replies |
| --- | --- | ---: | --- | --- | --- |
| GPT-6-Luna high | .560 / .615 / .500 | .558 (.058) | 1 / 2 / 0 | 40 / 36 / 41 | 24 / 23 / 24 |
| GLM-5.3 high | .583 / .615 / .667 | .622 (.042) | 2 / 2 / 4 | 16 / 13 / 14 | 23 / 23 / 21 |
| GLM-5.3-Flash high | .692 / .560 / .667 | .640 (.070) | 4 / 1 / 4 | 11 / 41 / 40 | 21 / 23 / 21 |

The first-run Flash fixed-report advantage is not stable across the three runs.
No model ranking is claimed beyond these twelve inputs and budgets.

## Controlled source replay

Among the original 39 subjects, thirteen have a retained native checker whose
code differs from its starting checker. The first three sorted IDs are G01,
G08 and G09. Their selected source variants were frozen before these new scans:
original source, a local variable rename, insertion of an inert `(void)0;`, and
an equivalent guard spelling. Every variant was applied on the vulnerable and
fixed revisions without changing the retained checker or calling a model.
Three checker arms on three cases and four source forms give 72 arm-side
measurements, with 64 distinct deterministic scans after safe reuse.

The original-source controls reproduce the recorded counts. For the native arm,
renaming and the no-op leave counts unchanged in these three cases. The guard
rewrite changes G01 from 2/2 vulnerable/fixed reports to 0/0, G08 from 1/1
to 2/2, and leaves G09 at 1/1. Native vulnerable-warning presence is retained
in eight of nine case-change pairs. G01's reduced fixed-side count is *not* a
benefit because its vulnerable warning is also lost. Location/count changes
do not themselves prove a target-property warning. These are controlled source
forms for selected original subjects, not natural future revisions or a new
generalization cohort.

## File map and hash semantics

- `evidence/E4_REPEATED_SUMMARY.json`: all three repeats, subject rows, and
  frozen source-file bindings.
- `evidence/E4_FINAL_INDEPENDENT_VERIFICATION.json`: independent raw/statistical
  verification receipt; 15,746 original bindings checked.
- `evidence/e4-new/`: 72 new trial roots, attempt exchanges and retained code.
- `evidence/refined-replay-v2/`: source-replay matrix, interpretation, predeclared
  contract, summaries and execution checks.
- `evidence/source-variants/`: frozen source files and variant manifest.
- `evidence/source-replay-scans/`: results and raw analyzer scan output used by
  the source-replay matrix.
- `EXPORT_MANIFEST.json`: per-file original/public SHA-256 and privacy edits.

The public path replacements change text bytes; historical source hashes still
refer to the original frozen workstation files. `verify_recorded.py` checks
published bytes and reported arithmetic, not access to the private workstation
or independent semantic correctness of each diagnostic.
