# SemWeaver FSE 2027 review package

This anonymous package supports inspection of the paper's **patch-related
false-positive reduction** results. It does not claim whole-kernel detection
throughput, vulnerability-class generalization, or a statistically significant
population effect. The 12 refined checkers are the complete fixed-noisy
stratum of a frozen 39-checker materialized cohort; they are not a random
sample of all generated checkers.

## Quick verification (no network, model, or Linux build)

From this package directory:

```bash
python3 scripts/verify_v7_review_package.py .
python3 scripts/summarize_v7_repeats.py \
  --repeats-root evidence/repeats_v7 \
  --screening inputs/e1/e2_screen1.csv \
  --output /tmp/semweaver-repeat-check.json
```

The first command checks every packaged file's SHA-256, redaction crosswalk,
12 frozen checker/patch inputs, Linux source blob bindings, all 37 strictly
internal Clang raw-output bindings, all three repeated paired outcomes, and
the separate ten-candidate expansion screen. The second recomputes descriptive
three-repeat statistics from the packaged records. No private credentials are
included or required for either command.

## What the numbers mean

Each table cell is `PDS successes / fixed-side alerts retained / actual model
calls`. PDS requires a vulnerable-side alert and no fixed-side alert on the
same patch-related build-object scope. All three methods had the same
*per-case call ceiling* within a repeat, inherited from the actual KNighter
run; successful SemWeaver cases can stop before that ceiling.

| Repeat | KNighter | SemWeaver native | SemWeaver without internal records |
| --- | ---: | ---: | ---: |
| 1 | 3 / 28 / 25 | 6 / 23 / 17 | 3 / 26 / 16 |
| 2 | 2 / 27 / 26 | 5 / 23 / 21 | 3 / 28 / 20 |
| 3 | 6 / 22 / 29 | 7 / 18 / 16 | 3 / 26 / 23 |

The 37 initial fixed-side alerts are object-level counts, not independent
classification examples. The paper's secondary F1 uses 78 patch-version
decisions across all 39 materialized checkers. KNighter behavior-PDS candidates
passing a separate structural review number 3, 1, and 5 by repeat; the native
SemWeaver counts are 6, 5, and 7. Review findings are quality warnings, not a
reason to erase valid paired scan outcomes. The repeated decodes use the same
development-informed 12 subjects, so they must not be pooled as 36 independent
cases. The exact within-repeat paired tests are not significant.

## Package map

- `source/SemWeaver/`: pinned tool, prompts, tests, KNighter adapter, and
  experiment drivers. The bundled KNighter code keeps its upstream Apache-2.0
  license and attribution. This SemWeaver review snapshot has **no
  project-level reuse license**.
- `source/manuscript/` and `paper/main.pdf`: the reviewed, anonymous paper
  source and compiled PDF rebased from the supplied original Overleaf draft.
- `inputs/e1/cases/`: twelve starting checkers, patches, metadata, and
  historical context. `inputs/vulnerable_source/` holds minimal public Linux
  blobs needed for offline source-scope binding checks.
- `evidence/semweaver_treatment_evidence_cohort_v3/`: 70 provenance-labelled
  records across 12 cases: 37 Clang analyzer-internal, 18 analyzer-output,
  and 15 source-derived. Internal records bind Clang 18
  `debug.DumpCFG`/`debug.DumpCallGraph` raw dumps, revision, file, and function.
  Source windows are never relabelled analyzer-internal.
- `evidence/repeats_v7/`: three actual KNighter loops, independent zero-model
  postvalidations, native/no-internal SemWeaver editor exchanges and candidate
  checkers, per-attempt paired results and build/validation logs, frozen
  manifests, protocol amendment, and aggregate summary.
- `evidence/expansion_*`: a separate screen of ten additional historical
  candidates. Every original `checker-final.cpp` was empty, so the
  hash-bound `checker-repaired.cpp` substitution is explicit. This screen is
  **not** silently added to the primary 39/12 comparison denominator.
- `evidence/e2_legacy_audit_v1/`: provenance audit of the old CSA/CodeQL
  table, which contains manual-path rows and is not an automatic-effect result.
- `evidence/repeats_v7/case19_patch_local_probe/`: post-hoc, development-
  informed fixture probe. The candidate passes unrelated-statement insertion
  and enclosing-function rename, but misses a consistently renamed cleanup
  callback. It is not a held-out robustness or generalization result.
- `ARTIFACT_MANIFEST.json` and `REDACTIONS.json`: packaged hashes and a
  digest crosswalk for path/account-only redactions. Raw scientific outcomes
  and candidate code are retained; redacted paths are not runtime locations.

## Re-execution boundary

Full model reruns require a user-supplied compatible endpoint, public Linux
revisions, the pinned Clang 18 environment, and substantial computation. A
floating model alias is not a byte-identical model snapshot. Compare frozen
candidate validation independently from stochastic regeneration. HTTP 429 and
other provider interruptions are infrastructure failures, not checker misses;
the three reported repeats contain no observed 429.

Reviewer-facing documentation is English. Historical raw files preserve some
upstream comments and tool strings in their original language; those are
scientific records, not instructions or translated model responses. No raw
exchange is reconstructed where an older run lacked it. The original E1
follow-up's six manual completions and the old five-case/model studies are
separate from the three repeated automatic comparison numerators.
