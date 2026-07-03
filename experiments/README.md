# Experiment Source

This directory contains experiment drivers, support queries, figure scripts,
baseline-integration source, and sample manifest CSVs. It does not include generated
checkers, scan reports, result tables, CodeQL databases, Linux/LLVM checkouts,
full sample worktrees, model caches, or API keys.

Runtime experiment data belongs under `artifacts/`:

- `artifacts/experiments/cross_backend/`: SemWeaver sample manifests, runs, logs, tables, and figures.
- `artifacts/experiments/knighter/`: Knighter experiment runs and derived summaries.
- `artifacts/external/`: large external trees such as Linux, LLVM, host dependencies, and downloaded datasets.

The core tool can be installed and smoke-tested without these data directories.
The included sample material is manifest-only.
Paper-scale result reproduction requires the separate artifact data package that
will populate `artifacts/`.

## Included Source

- `samples/`: sanitized sample manifest CSVs. It currently contains 39-row
  Knighter experiment lists and 37-row cross-backend lists, but not the corresponding worktrees.
- `cross_backend/`: cross-backend experiment management scripts, CodeQL smoke query, and figure-generation scripts.
- `knighter/baseline/`: anonymized Knighter baseline source subset needed by the integration scripts.
- `knighter/experiment/`: Knighter ablation, robustness, and limited-generalization driver scripts.

## Basic Checks

```bash
python3 -m src.main experiment init
python3 -m src.main experiment audit \
  --root artifacts/experiments/cross_backend \
  --manifest experiments/samples/cross_backend/profiles/samples.csv \
  --sample-id vul4c_cwe369_libtiff_cve20177595
```

The audit command reads the source manifest and sample files from
`experiments/samples/` and `artifacts/`, then writes runtime audit output under
`artifacts/`.

For the Knighter smoke profile after the artifact package has populated
`artifacts/research/knighter/cases/`:

```bash
./scripts/knighter_smoke.sh setup
./scripts/knighter_smoke.sh audit
```
