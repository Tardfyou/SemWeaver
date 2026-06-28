# Artifact Structure

This source release is organized as a standalone tool repository.

## Included

- Core framework source under `src/`.
- Prompt templates under `prompts/`.
- Small knowledge seeds under `data/knowledge/`.
- Default configuration under `config/`.
- Setup and helper scripts under `scripts/`.
- A small smoke fixture under `tests/tiny_buffer_lab/`.
- Documentation under `docs/`.

## Not Included

The following are intentionally excluded from this source release:

- Experiment directories and paper result tables.
- Dataset materialization scripts and curated benchmark checkouts.
- CodeQL databases, generated detector outputs, logs, and scan reports.
- Downloaded embedding/model caches.
- API keys, private endpoint URLs, local absolute paths, and user-specific IDE settings.

## Runtime Outputs

Expected generated paths:

- `output/`: generated detectors, reports, refinement runs.
- `logs/`: log files.
- `codeql_dbs/`: CodeQL databases.
- `codeql_cache/`: CodeQL cache files.
- `pretrained_models/`: downloaded embedding models.

These paths are ignored by Git.
