# Installation

## System Requirements

Recommended environment:

- Linux x86_64.
- Python 3.10 or newer.
- `clang`/`clang++` and LLVM/Clang development headers for CSA detector compilation.
- CodeQL CLI for CodeQL query validation.
- Docker or Docker Compose only if using the optional ChromaDB-backed knowledge base.

The default configuration expects LLVM 18 at `/usr/lib/llvm-18`. Override paths with environment variables:

```bash
export LLVM_DIR=/path/to/llvm
export CLANGXX=/path/to/clang++
export CODEQL=/path/to/codeql
```

## Python Environment

```bash
python3 -m venv .venv
source .venv/bin/activate
python3 -m pip install --upgrade pip
python3 -m pip install -r requirements.txt
```

## Optional Setup Script

The setup script installs Python dependencies, creates output/cache directories, and can optionally install CodeQL:

```bash
bash scripts/setup.sh
bash scripts/setup.sh --install-codeql
```

The script does not install or configure API keys.

## Optional Knowledge Base

SemWeaver can run without ChromaDB, but RAG-backed knowledge search is enabled when ChromaDB is available:

```bash
docker compose up -d
python3 scripts/import_knowledge.py
```

Generated ChromaDB data, model downloads, CodeQL databases, logs, and outputs are ignored by Git.
