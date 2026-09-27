# SemWeaver — anonymous review artifact

This repository contains the final FSE 2027 review package for **SemWeaver: Provenance-Guided Refinement of LLM-Generated Vulnerability Checkers**.

- [Paper](review-package/paper/main.pdf)
- [Artifact guide and reproduction requirements](review-package/README.md)
- [Corrected 39-case results](review-package/results/corrected/SUMMARY.csv)
- [Evidence scope and limitations](review-package/docs/EVIDENCE_SCOPE.md)
- [Automatic motivating example and redraw materials](review-package/evidence/motivating_example/)

From a fresh checkout, verify the package without model calls:

~~~bash
cd review-package
python3 scripts/verify_corrected_review_package.py .
~~~

The current results use all 39 frozen generated CSA subjects, the actual KNighter refinement loop, a matched 15-case noise-reduction comparison, full-cohort ablation, three editor configurations and three decodes on a retained 12-subject subset. PDS/F1 are **patch-version warning metrics**, not exhaustive target-vulnerability localization or deployment precision. Superseded raw records remain for audit; only `results/corrected/` defines the current effectiveness totals.

All explanatory material is English. No project-level license is granted; this material is supplied for review. Upstream license notices remain in force.
