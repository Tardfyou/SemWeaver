# Experiment Source

This directory contains experiment drivers, support queries, figure scripts,
and baseline-integration source only. It does not include datasets, manifests,
generated checkers, scan reports, result tables, CodeQL databases, Linux/LLVM
checkouts, model caches, or API keys.

Runtime experiment data belongs under `artifacts/`:

- `artifacts/experiments/v2/`: SemWeaver sample manifests, runs, logs, tables, and figures.
- `artifacts/experiments/knighter/`: baseline/E2 runs and derived summaries.
- `artifacts/external/`: large external trees such as Linux, LLVM, host dependencies, and downloaded datasets.

The core tool can be installed and smoke-tested without these data directories.
Paper-scale reproduction requires the separate artifact data package that will
populate `artifacts/`.

## Included Source

- `v2/`: SemWeaver experiment management scripts, CodeQL smoke query, and figure-generation scripts.
- `knighter/baseline/`: anonymized KNighter baseline source subset needed by the integration scripts.
- `knighter/e2/`: E2 ablation, robustness, and limited-generalization driver scripts.

## Basic Checks

```bash
python3 -m src.main experiment init
python3 -m src.main experiment audit --root artifacts/experiments/v2
```

The audit command expects a populated manifest and sample files under
`artifacts/experiments/v2`.
