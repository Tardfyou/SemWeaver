# SemWeaver V2 Experiment Drivers

These scripts manage the patch-local generation/refinement experiments. They
are source-only; datasets and generated outputs must be placed under
`artifacts/experiments/v2`.

## Layout

- `scripts/materialize_vul4c_samples.py`: materializes selected Vul4C samples into the artifact workspace.
- `scripts/materialize_git_commit_samples.py`: materializes selected upstream Git samples into the artifact workspace.
- `scripts/audit_sample_envs.py`: prepares and smoke-audits selected sample environments.
- `scripts/audit_patch_targets.py`: checks whether patch target files are present and analyzable.
- `scripts/run_generate_batch.py`: runs approved generation samples one at a time.
- `support/codeql_smoke/`: minimal CodeQL query used by environment audits.
- `figures/`: scripts that rebuild figures from CSV tables in `artifacts/experiments/v2/tables`.

## Default Workspace

All scripts default to:

```text
artifacts/experiments/v2
```

That workspace should contain `manifests/`, `datasets/`, `runs/`, `logs/`, and
`tables/` after the artifact data package is unpacked or materialized.

## Typical Commands

```bash
python3 -m src.main experiment init
python3 experiments/v2/scripts/audit_sample_envs.py --limit 1 --skip-csa --skip-codeql
python3 experiments/v2/scripts/run_generate_batch.py --resume --limit 1
```

Materialization scripts need external datasets or repository access and should
be run only after the artifact data instructions are available.
