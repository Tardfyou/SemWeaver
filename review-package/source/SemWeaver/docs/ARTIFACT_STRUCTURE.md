# Artifact Structure

This source release is organized as a standalone tool repository.

## Included

- Core framework source under `src/`.
- Prompt templates under `prompts/`.
- Small knowledge seeds under `data/knowledge/`.
- Default configuration under `config/`.
- Setup and helper scripts under `scripts/`.
- Source-only experiment drivers and sample manifest CSVs under `experiments/`.
- Placeholder artifact data directory under `artifacts/`.
- A small smoke fixture under `tests/tiny_buffer_lab/`.
- Documentation under `docs/`.

## Not Included

The following are intentionally excluded from this source release:

- Paper result tables.
- CodeQL databases, generated detector outputs, logs, and scan reports.
- Downloaded embedding/model caches.
- API keys, private endpoint URLs, local absolute paths, and user-specific IDE settings.

## Runtime Outputs

Expected generated paths:

- `output/`: generated detectors, reports, refinement runs.
- `artifacts/experiments/`: staged experiment manifests, materialized sample worktrees, runs, logs, tables, and figures.
- `artifacts/research/knighter/` and `artifacts/experiments/cross_backend/`: optional full sample environments supplied by the artifact package.
- `artifacts/external/`: large external dependencies such as Linux, LLVM, and host sysroots.
- `logs/`: log files.
- `codeql_dbs/`: CodeQL databases.
- `codeql_cache/`: CodeQL cache files.
- `pretrained_models/`: downloaded embedding models.

These paths are ignored by Git.

## Source Sample Profiles

Sample manifest CSVs live under `experiments/samples/`. They include sanitized
sample identities, run flags, and artifact-relative paths. They must not contain
full sample worktrees, generated reports, result tables, scan logs, model
outputs, cached databases, or local credentials.
