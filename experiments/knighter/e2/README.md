# KNighter E2 Drivers

These scripts operate on E2 case directories and baseline/refined checker
outputs. They are source-only and expect their inputs under
`artifacts/experiments/knighter/e2`.

Included drivers:

- `prepare_ablation_cases.py`: creates controlled evidence-ablation inputs.
- `collect_ablation_result.py`: summarizes strict ablation scan logs.
- `collect_model_result.py`: summarizes model-robustness scan logs.
- `run_refined_fixed_fullscan.py`: runs selected refined checkers against fixed kernel revisions.

The scripts assume the external Linux/LLVM environment is available under
`artifacts/external/` unless overridden on the command line.
