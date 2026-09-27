# Complete 39-case KNighter system baseline

This directory binds the actual KNighter report-refinement run and the
zero-report branch to the same 39 automatically generated CSA starting
checkers used by SemWeaver. The starting paired screen is complete and
execution-valid: 14 vulnerable-hit/fixed-noisy checkers and 25 vulnerable
target misses. No starting checker meets strict PDS.

KNighter's actual report-triage/repair loop ran on all 14 cases with fixed
reports. It made 91 completed semantic model calls in total; independent
paired postvalidation accepted one PDS edit. The other 25 cases entered the
real zero-report branch of the packaged KNighter adapter. Each starting
checker compiled, but there was no fixed-side report to triage, so the branch
made zero model calls and left the checker unchanged. The upstream return
word `Perfect` means *no false-positive report to refine* here. It does not
mean that the vulnerable target was detected.

The hash-bound `RESULT.json` gives one vulnerable-positive and one
fixed-negative decision per case. Under adopt-only-on-independent-PDS:

| Portfolio | TP | FP | FN | TN | PDS | Fixed object alerts | F1 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Generated starting checkers | 14 | 14 | 25 | 25 | 0/39 | 71 | 0.4179 |
| KNighter system output | 14 | 13 | 25 | 26 | 1/39 | 70 | 0.4242 |

These are 78 patch-version decisions, not individual-alert precision or
unseen-vulnerability recall. The full-39 comparison is a *system* comparison:
on the 25 target misses, KNighter's false-positive refinement subroutine
does not attempt target-hit recovery. The direct same-task report-refinement
comparison is the 14-case fixed-noisy stratum. Per-case model calls,
refinement applicability, selected candidate hashes, and output alerts are
retained in `RESULT.json`.

Bindings: `RESULT.json` SHA-256 is
`c1d4a344d715018a9e27a0ccc310ff970f3702028859e42489b03613468be219`.
The underlying 25-case no-report batch SHA-256 is
`dd732840e8d6c0dfc13f04a76c9f4ad2db1f79d93a8c43b3482da8ff2dc053ee`;
the 14-case strict postvalidation SHA-256 is
`904a73f30c07ca55d3a07a9882dd7b5a4799a081c5baf7ab1d214e37e4c6c52f`.
The full-system summary script checks those files and the frozen 39-case
starting screen before computing any rate. No historical hand-completed
checker or manual result is included.
