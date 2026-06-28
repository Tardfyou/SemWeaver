# SemWeaver

SemWeaver is a patch-guided framework for generating and refining static-analysis detectors. It takes a security patch, extracts patch-relevant evidence from the target source tree, and helps synthesize or refine detectors for Clang Static Analyzer (CSA) and CodeQL.

This repository contains the project source code, prompts, configuration, knowledge seeds, setup scripts, a small smoke-test lab, and source-only experiment drivers. It intentionally does not include bulk datasets, materialized sample checkouts, cached databases, model caches, scan outputs, result tables, API keys, or author-identifying metadata.

## Repository Contents

- `src/`: SemWeaver implementation.
- `prompts/`: prompt templates used by generation, evidence, and refinement stages.
- `data/knowledge/`: small static knowledge seeds used by optional RAG import.
- `config/config.yaml`: default environment-variable based configuration.
- `scripts/`: setup, ChromaDB/RAG, CodeQL, and CSA helper scripts.
- `experiments/`: source-only artifact experiment drivers and baseline-integration code.
- `artifacts/`: placeholder for future artifact data packages and generated experiment outputs.
- `tests/tiny_buffer_lab/`: minimal C buffer-bound smoke fixture.
- `docs/`: installation, usage, configuration, and structure documentation.

## Quick Start

```bash
cd SemWeaver
python3 -m venv .venv
source .venv/bin/activate
python3 -m pip install -r requirements.txt

export SEMWEEVER_LLM_PROVIDER=openai
export SEMWEEVER_MODEL=gpt-4.1
export OPENAI_API_KEY="<your key>"

python3 -m src.main --help
```

For optional local knowledge retrieval:

```bash
docker compose up -d
python3 scripts/import_knowledge.py
```

## Documentation

- [Installation](docs/INSTALL.md)
- [Usage](docs/USAGE.md)
- [Configuration](docs/CONFIGURATION.md)
- [Artifact Structure](docs/ARTIFACT_STRUCTURE.md)
- [Experiment Source](experiments/README.md)
- [Security Notes](SECURITY.md)

## Scope

The included smoke fixture is for checking that the toolchain, prompts, and CLI are wired correctly. Experiment driver source is included, but manifests, datasets, result tables, scan outputs, and generated artifacts should be added later under `artifacts/` only.
