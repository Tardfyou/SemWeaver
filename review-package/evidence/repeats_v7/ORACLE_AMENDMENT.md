# V7 paired-validation oracle amendment

This amendment is prospective for rep02 and rep03 treatments. It does not
rewrite the frozen V7 protocol or any original result. The prior validator
suppressed vulnerable/fixed scans whenever a static artifact-review heuristic
reported a finding. In rep02 KNighter case 08 this converted a compilable
candidate into `unscored`, conflating structural quality with executable
paired detection. The primary PDS oracle is now **compiles, scans both pinned
versions, alerts on vulnerable, silent on fixed**. Static artifact review is
reported separately as `review_passed` and `adoptable_with_review_gate`.

The validator-only source revision is
`5685b87ffb4e9942ac50b1ab917e657a623872cb` (114 tests passed). It adds
`--review-policy diagnostic` as the primary policy and preserves the legacy
`gate` option. No model, prompt, input corpus, decoding settings, or subject
order changed. The original rep01 treatment results remain untouched: all 22
external frozen-validation results across native and no-internal arms have
`artifact_review.passed=true`, so this policy change does not alter their PDS
outcomes. Their original manifest binds implementation revision `3114fc9`.

Zero-model KNighter revalidation was written to fresh directories:

| Replicate | Primary PDS / subjects | Unscored | Review-failed refined | Result SHA-256 |
| --- | ---: | ---: | ---: | --- |
| rep01 | 3/12 | 0 | 0 | `6ebe63e6b546a3563470256d82769071eef3152737b44a411923bf323d2e6bba` |
| rep02 | 2/12 | 0 | 1 | `1aafb1e88d10f45bd0fb1d5e3729c6cee730c959b70c66930bb96225ad2e32ce` |

Rep01 reproduces its prior 3/12 baseline. Rep02 case 08 has valid paired
execution with 2 vulnerable and 0 fixed alerts, but fails the independent
structural review because a declared `ProgramState` has no state reads/writes.
Rep02 case 14 also has PDS behavior and passes review. Thus the rep02
review-gated adoption count is 1/12, while the primary behavior-only PDS
count is 2/12; neither is an ecological generalization result. The former
legacy rep02 strict result remains in `rep02/knighter_strict/` for audit.

Treatments after this amendment must bind the new rep02 strict summary
*before* any treatment model call. Both arms must use the same code revision,
inputs and per-case caps. Any further oracle change requires a new amendment
and fresh result directories.
