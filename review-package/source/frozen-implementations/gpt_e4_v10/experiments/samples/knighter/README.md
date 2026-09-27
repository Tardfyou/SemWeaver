# Knighter Sample Manifests

This directory contains sanitized Knighter experiment sample manifest CSVs only. The
default smoke profile names one minimal case expected from the external artifact
package:

- `08_768f17fd25e4_Integer_Overflow`: an integer-overflow patch-local checker
  case for `drivers/gpu/drm/i915/i915_hwmon.c`.

The source repository does not include patch diffs, seed checkers, Linux
worktrees, or the 08 smoke source stubs. Those belong under
`artifacts/research/knighter/cases/` after the artifact package is unpacked. The CSVs
exclude generated refinement outputs, validation logs, result tables, alert
counts, model outputs, and historical scan statistics.

Additional sanitized sample-list profiles are available under `profiles/`:

- `knighter_full_manifest.csv`: 39-case Knighter candidate list.
- `knighter_materialized_cases.csv`: 39-case artifact case list.
- `knighter_refinement_samples.csv`: 12 selected refinement sample identities.
- `knighter_ablation_samples.csv`: 5 ablation sample identities and roles.
- `knighter_qualitative_samples.csv`: 3 qualitative sample identities and focus tags.

These profiles omit alert counts, report counts, result statuses, and generated
artifact paths. The referenced case data is expected under
`artifacts/research/knighter/cases/`.

## Smoke Flow

From the repository root:

```bash
./scripts/knighter_smoke.sh setup
./scripts/knighter_smoke.sh audit
```

These commands require the artifact package to provide
`artifacts/research/knighter/cases/08_768f17fd25e4_Integer_Overflow/source/vulnerable`
and `source/fixed`. Without that package, the script stops with a setup
message.

To run the smoke experiment after external Linux/LLVM assets and an LLM provider
are configured:

```bash
./scripts/knighter_smoke.sh run
```

The runtime workspace is created under:

```text
artifacts/experiments/knighter/smoke/
```

## Extending The Smoke Set

Add another case to the external artifact package under
`artifacts/research/knighter/cases/<case_id>/`, then add a row to
`profiles/smoke.csv`. The setup script stages the selected sample ID into
`artifacts/experiments/`; use `SEMWEEVER_KNIGHTER_SMOKE_SAMPLE` to switch cases.
