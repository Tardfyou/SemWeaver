# Cross-Backend Sample Profiles

This directory contains sanitized cross-backend sample profiles only:

- `profiles/samples.csv`: 37-row main sample manifest with staging paths and run flags.
- `manifests/samples.csv`: same sanitized runner manifest for tools that expect a manifest directory.
- `profiles/vul4c_seed_selection.csv`: 32-row Vul4C seed selection list.
- `profiles/supplement_git_selection.csv`: 5-row supplemental upstream Git selection list.

The profiles point to the expected artifact package layout under
`artifacts/experiments/cross_backend/datasets/curated/`. They do not include materialized
sample worktrees, generated detectors, scan logs, result tables, figures, API
keys, or reviewer-identifying fields. Run outputs should be directed to
`artifacts/experiments/cross_backend/`.
