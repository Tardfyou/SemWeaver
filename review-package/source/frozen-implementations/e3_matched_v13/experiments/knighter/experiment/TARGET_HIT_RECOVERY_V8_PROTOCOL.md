# V8 target-hit recovery extension (separate from precision refinement)

The independent 39-case generate-only screen also identifies starting checkers
with valid execution but no vulnerable-side hit. These are **not** eligible for
the primary false-positive-refinement comparison: without a hit, reducing
fixed-side alarms can only make the detector quieter, not better. Their full
denominator and selection order are inherited from the frozen screen result.

The `target_hit_recovery` mode starts from exactly the screened checker whose
SHA-256 matches a valid paired `RESULT.json`. It requires a starting
vulnerable-side miss; continuation attempts must bind to the preceding
candidate and paired result. The agent receives the same patch and provenance-
gated analyzer evidence as the precision mode, but its explicit task is to
recover a mechanism-level vulnerable trigger **and** retain fixed-side
silence. It must not insert an exact patch-line, function-name, or statement-
adjacency fingerprint to force a target alert.

The strict endpoint remains PDS: at least one alert on the vulnerable
revision and zero alerts on the fixed revision in the independent Clang 18
paired oracle. Vulnerable hit with residual fixed alerts is partial coverage
recovery, not PDS; reduced fixed-side alerts without vulnerable hit is not
recovery. Compile failures, execution errors, evidence-gate abstentions, and
transport interruptions keep distinct statuses. We report the full starting-
miss denominator, attempted and valid candidates, recovered hits, PDS,
fixed-side alert counts, calls/tokens, and case-level binary precision/recall/
F1. The run uses the same eight semantic-call ceiling, 32,768 output-token
ceiling, and temperature 0.0 as the precision study.

All code edits must be model-produced and pass the usual LSP, structural
review, and compilation gates. Metamorphic tests vary local/callback names,
unrelated statements, branch presentation, and negative object/guard
relations while preserving the patch's API contract. Development-informed
tests and genuinely held-out tests are labeled separately. A patch-local PDS
or fixture pass is not evidence of whole vulnerability-class recall or
cross-project generalization. Until the full cohort and independent probes
are complete, this mode is an exploratory tool capability, not an FSE main
result.
