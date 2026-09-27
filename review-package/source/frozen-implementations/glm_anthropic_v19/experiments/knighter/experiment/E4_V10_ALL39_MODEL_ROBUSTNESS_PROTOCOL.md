# E4: all-39 automatic model robustness protocol

E4 uses exactly the frozen 39 automatically generated CSA checkers and their
independent starting paired screen: 14 fixed-noisy vulnerable hits and 25
vulnerable target misses. There is no manual candidate, hand-completed edit,
post-outcome case selection, or replacement of a failed subject.

For every model, the native-evidence SemWeaver agent receives the same
hash-bound patch, starting checker, source snapshot, and Clang CFG/call-graph
records. The 14 fixed-noisy cases also receive summaries of the same frozen
CSA fixed-report IDs used by the primary comparison. The 25 target misses use
the target-hit-recovery objective and their frozen 0-hit starting feedback.
The one case lacking eligible strict internal evidence is an explicit
abstention in every native-evidence model condition.

The separate model conditions are GPT-6-Luna with high reasoning effort,
GLM-5.3, and GLM-5.3-Flash. Each case allows at most two successful semantic
model calls; every changed, compiled candidate is independently validated
under Clang 18 before a later permitted model call. The V10 end-of-budget rule
still validates a model-edited
candidate if its last edit compiled before the second-call ceiling; the
external validator makes the final execution and structural judgment. A
provider 429, empty length-limited answer, or timeout is unscored and rerun
only in a fresh, hash-bound directory.

The semantic call ceiling is matched, but the **configured output-token
ceiling is not a token-matched model comparison**: GPT uses 16,384 tokens;
GLM uses 65,536 because forced reasoning previously consumed a 32,768-token
ceiling without returning a parseable answer. Actual prompt, completion, and
total token usage, latency, and any interruptions are retained. We report
per-model full-39 PDS, vulnerable-hit recovery, hit-preserving fixed-alert
reduction, and fixed-alert burden, including 0, 1, 2--5, and >5 fixed-alert
bands. These are patch-pair outcomes, not unseen-code generalization.

The initial objective is one complete run per model across all 39 subjects.
It establishes breadth, not decoding variance. Repeated independent decodes,
if run, must use the same entire frozen 39 and distinct output directories;
they are summarized by per-case success frequency and never pooled as new
independent patches. E4's two-call profile is separate from E3's eight-call
native/no-internal ablation and from the closest KNighter system baseline.
