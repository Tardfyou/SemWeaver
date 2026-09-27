# Generated-only CSA cohort: report-matched follow-up protocol

This protocol is a documented follow-up to V8, not a replacement for its raw
records. It was motivated by an input asymmetry observed while V8 was running:
KNighter received actual fixed-side CSA HTML warnings and path events, whereas
the V8 SemWeaver precision arm received only the paired alert counts. The V8
results remain diagnostic and are never overwritten with V9 outcomes.

## Scope and denominators

The upstream artifact inventory has 286 generated checker records over 60
commits. We froze and independently paired-screened **39** generated-only CSA
checkers, one per selected commit. All 39 executed: 14 hit the vulnerable
version but warned on the fixed version, and 25 missed the vulnerable target.
No additional generated records are being screened. The incomplete earlier
286-record screen is stopped and contributes no rate or denominator.

The 14 fixed-noisy cases form the only direct KNighter refinement comparison:
they have the fixed-side reports required by KNighter's own false-positive
loop. The 25 target misses are evaluated only as a separate SemWeaver
target-hit-recovery task and a same-agent no-internal ablation. KNighter's
false-positive loop is not charged with failing to repair cases to which it
does not apply. All 39 cases are Clang Static Analyzer (CSA) checkers. CodeQL
development probes are separate E2 cases and never enter this denominator.

## Fair-input refinement comparison

Both methods receive the same generated starting checker, Linux patch pair,
patch-related build-object scope, fixed-side warning IDs (up to the same first
five), GPT-6-Luna-high Responses model, 16,384 output-token ceiling per call,
and independent Clang 18 paired validator. Both have an eight-successful-call
ceiling per case in the equal-configured-budget comparison; actual calls and
tokens are reported separately because the methods may stop early. The
packaged KNighter adapter
corrects its report-triage patch argument to the actual frozen patch; this
correction is disclosed and is not described as byte-identical upstream code.
After KNighter completes all 14 cases, a separate conservative sensitivity
condition freezes its *actual* per-case successful call counts as smaller
ceilings for both V9 SemWeaver arms. This condition must not be pooled with
the equal-ceiling runs. SemWeaver stops early on accepted PDS. Method-specific
prompts and local repair steps are not claimed identical.

V9 passes deterministic compact summaries of the same first five frozen HTML
reports to both SemWeaver precision arms. Each report ID and original HTML
SHA-256 is bound in the batch and run manifests. Report descriptions and path
events are *analyzer output*, not analyzer-internal evidence. The native arm
additionally receives audited, raw-hash- and patch-bound Clang CFG/call-graph
records; the no-internal arm does not. Both keep the same patch, bounded source
context, report summaries, model, call ceilings, and paired validation.
One of the 39 cases lacks eligible strict internal records and is an explicit
native-arm abstention; the no-internal arm may still run it.

## Scoring and stopping rule

Only a changed, executable checker with an independent vulnerable-side hit
and zero fixed-side target warnings is adopted. A vulnerable hit with fixed
noise is partial progress, never PDS; fixed silence with a lost vulnerable
hit is validity loss. A model refusal, compilation failure, or complete call
budget is retained with its own status. Provider 429, output-token exhaustion,
and infrastructure timeouts are unscored interruptions and must be repaired
or rerun in fresh output directories; they are not method failures.

The 39-case portfolio reports each starting checker and its selected
adopt-only-on-PDS output. Each patch contributes one vulnerable positive and
one fixed negative, so micro precision, recall, and F1 use exactly 78 binary
patch-version decisions. Raw alert totals are separate descriptive counts,
not independent classification examples. As a secondary diagnostic, we also
report the best independently validated candidate within each frozen budget
that *retains the vulnerable-side hit* and lowers the fixed-side alert count,
even if fixed-side alerts remain. Such a case is explicitly partial reduction,
not PDS or an adopted checker; silencing both sides earns no reduction credit.
For target-miss cases, a recovered vulnerable hit with residual fixed noise
is likewise reported separately from PDS. The summary script refuses partial
39-case runs and checks batch, candidate, and external validation hashes
before computing any portfolio rate. Direct KNighter-vs-SemWeaver PDS and
fixed-warning comparisons use the 14 applicable cases only; full-39 portfolio
figures must be labeled as different operational scopes, not a direct
same-task head-to-head on the 25 misses.

No manually edited checker is an automatic treatment result. Post-hoc
metamorphic probes are development diagnostics, not unseen-code estimates.
GLM-5.3-Flash and GLM-5.3 robustness runs, if transport-complete, are
reported as separate model conditions rather than pooled with GPT results.
