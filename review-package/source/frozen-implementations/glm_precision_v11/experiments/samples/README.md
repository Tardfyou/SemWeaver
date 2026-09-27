# Experiment Sample Manifests

This directory contains sample manifest CSVs only. It does not contain full
sample worktrees, generated results, result tables, scan reports, model outputs,
CodeQL databases, LLVM builds, Linux checkouts, or API keys.

Full sample environments and runtime outputs belong under `artifacts/`.

Included profiles:

- `knighter/profiles/smoke.csv`: one minimal Knighter smoke sample.
- `knighter/profiles/knighter_*.csv`: sanitized Knighter experiment sample lists.
- `cross_backend/profiles/*.csv`: sanitized cross-backend sample selection and manifest lists.
- `cross_backend/manifests/samples.csv`: 37-row cross-backend runner manifest.

Profile CSV files keep sample identity, paths, run flags, and selection
metadata. They intentionally omit result tables, aggregate statistics, scan
logs, model outputs, generated detectors, and reviewer-identifying fields.
Paths point to the expected artifact package layout under `artifacts/`.
