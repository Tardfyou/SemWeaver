# Verified main comparison: all 39 subjects

The selected whole implementation is v43, revision
`16b57931fb105e122897fb1e063e11c366e7137e`. The authoritative aggregation is
`FINAL39_SUMMARY.json`. A separate arithmetic and selected-artifact check verified
all 117 method-subject pairs, their checker identities, validation hashes and
normal paired executions. Repeated ablation is now complete; the model
configuration matrix remains separate and unfinished.

## Full cohort

These metrics count warning-positive patch versions, not independently
adjudicated target vulnerabilities. Fixed-side reports measure warning burden;
they are not 123 distinct confirmed false-positive bugs.

| Method | Vulnerable versions with warnings | Fixed versions with warnings | Vulnerable-positive / fixed-silent pairs | Fixed-side reports | Version-level F1 | Model responses |
|---|---:|---:|---:|---:|---:|---:|
| SemWeaver native | 23 | 18 | 5 | 123 | 0.5750 | 150 |
| SemWeaver no-internal | 21 | 13 | 8 | 68 | 0.5753 | 133 |
| KNighter | 15 | 13 | 2 | 69 | 0.4478 | 179 |

All three methods retain warning signals on the 15 initially warning-positive
vulnerable versions. Native recovers a warning signal on 8 of the 24 initially
silent vulnerable versions; no-internal recovers 6, and KNighter recovers none
under its actual no-report workflow. These are not verified target-recall counts.

Relative to no-internal, native has 6 positive warning changes, 5 comparative
disadvantages and 28 ties/other outcomes. A comparative disadvantage means
that no-internal obtained a better retained result; it does not mean accepting
a worse candidate relative to the native arm's own retained checkpoint.
Its extra warning coverage comes with more
fixed-side noise; aggregate F1 is effectively unchanged. Do not claim universal
native-evidence superiority or an overall reduction of fixed-side reports.

## Prespecified report-refinement stratum

The original 15 subjects warning-positive on both revisions form the secondary
precision-oriented stratum. No subject is added or removed based on new results.

| Method | Vulnerable versions with warnings | Fixed-silent pairs | Fixed-side reports | Version-level F1 |
|---|---:|---:|---:|---:|
| SemWeaver native | 15 | 2 | 44 | 0.6977 |
| SemWeaver no-internal | 15 | 2 | 68 | 0.6977 |
| KNighter | 15 | 2 | 69 | 0.6977 |

Native reduces report burden by 25 reports (36.2%) relative to KNighter and
24 (35.3%) relative to no-internal in this stratum, while retaining a version-level
warning signal. Binary fixed-version positivity and F1 are identical here.
Lower report burden does not prove retention of each target warning: G24's
target-alignment qualification remains unresolved. G12's noisy 73 fixed-side
reports remain in the full-cohort result and must not be excluded as an outlier.

The stratum's net reduction is concentrated in G24: its fixed-side reports
decrease from 27 to 2 against both comparators. Against no-internal, G14 adds
one report, yielding the net 24-report reduction. Against KNighter, G08/G21
each remove one and G14 adds two, leaving the net reduction at 25. This is
not a typical-case or broadly distributed reduction; retain the per-case table
and G24's unresolved target-retention limitation alongside the aggregate.

## Protocol and interpretation

Main methods share the requested model GPT-6-Luna high, starting checkers and
paired scanner. Response ceilings and actual termination are both recorded;
KNighter retains its five-report/two-outer-attempt/no-report policy. Two audited
one-response natural-stop reuses retain their originally recorded ceiling of
eight, not a fabricated ceiling of 32. Equal response ceilings are not equal
token or execution budgets.

The controlled-fixture retention guard is native-only; it rejected zero
candidates in the completed main chains. Its additional validation overhead is
still disclosed. Native dynamic evidence was usable on 103 of 150 unique
native-chain attempts; the remaining availability categories are recorded
separately, with no inference of safety from missing observations.

The supported conclusion is conditional: automatic refinement can recover
version-level warning signals and reduce report burden in the prespecified
refinement stratum. Native evidence offers benefits on some subjects, not a
general improvement in precision, F1 or warning burden.

## Complete repeated ablation

`REPEATED_ABLATION_STATUS.json` contains all12 original subjects and36 paired
decodes (72 condition cells; first decode from main,48 additional executions).
There are5 positive warning changes (4 recoveries,1 fixed-report reduction),
3 comparative disadvantages (2 higher fixed-report counts than control,
1 control-only recovery),
and28 ties/other outcomes. Decodes are not independent bugs.

The three disadvantages do not violate checkpoint retention. G08 repeat2
retains its initial2/2 while control improves to1/1. G14 repeat1 improves
from3/3 to2/2 while control reaches2/1. G38 repeat1 remains initially0/0
while control reaches1/0; native does not lose an existing starting hit.
The raw historical direction key `negative_lost_version_hit` in that last
comparison is preserved for reproducibility but must not be paraphrased as
native adopting a checker that lost its initial warning signal.

Five subjects tie in all three decodes; seven have mixed directions. No subject
has a positive native-versus-control change in all three decodes. G19 recovers
the vulnerable-version signal without fixed warnings in all three native
decodes, but no-internal also reaches15/0 in its third decode; therefore the
comparative benefit occurs in two of three, not three of three. G24's large
main-decode burden reduction does not repeat: subsequent pairs are3/3 versus
3/3 and27/27 versus27/27. Keep its target-retention qualification.

The repeated observations support conditional, sampling-dependent benefits,
not a stable universal advantage of native evidence. The limited-budget
three-configuration sensitivity still needs completion before final manuscript,
artifact and publication closure.
