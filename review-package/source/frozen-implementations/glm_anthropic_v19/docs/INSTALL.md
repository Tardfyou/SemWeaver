# Installation

## System Requirements

Recommended environment:

- Linux x86_64.
- Python 3.10 or newer.
- `clang`/`clang++`, `clangd`, and LLVM/Clang development headers for CSA detector compilation and LSP validation.
- CodeQL CLI for CodeQL query validation.
- Docker or Docker Compose only if using the optional ChromaDB-backed knowledge base.

The default configuration expects LLVM 18 at `/usr/lib/llvm-18`. Override paths with environment variables:

```bash
export LLVM_DIR=/path/to/llvm
export CLANGXX=/path/to/clang++
export CLANGD=/path/to/clangd
export CODEQL=/path/to/codeql
```

On Ubuntu-like systems, install LLVM/Clang with the package version that matches `LLVM_DIR`, for example:

```bash
sudo apt update
sudo apt install clang-18 clangd-18 llvm-18-dev libclang-18-dev
```

If the distribution installs versioned binaries only, point the environment variables at them:

```bash
export LLVM_DIR=/usr/lib/llvm-18
export CLANGXX=/usr/lib/llvm-18/bin/clang++
export CLANGD=/usr/lib/llvm-18/bin/clangd
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

After setup, check the command-line entry point and tool paths:

```bash
python3 -m src.main --help
${CLANGD:-/usr/lib/llvm-18/bin/clangd} --version
${CODEQL:-codeql} version
```

## Optional Knowledge Base

SemWeaver can run without ChromaDB, but RAG-backed knowledge search is enabled when ChromaDB is available:

```bash
docker compose up -d
python3 scripts/import_knowledge.py
```

The helper script performs the same ChromaDB setup and prepares the local embedding-model cache:

```bash
bash scripts/setup_rag.sh
python3 scripts/test_rag.py
```

The default embedding model is `sentence-transformers/all-MiniLM-L6-v2`. The first RAG run downloads it into `pretrained_models/` unless that cache is already populated. For offline use, pre-populate `pretrained_models/` with the Hugging Face cache layout before running `scripts/test_rag.py` or `scripts/import_knowledge.py`.

Generated ChromaDB data, model downloads, CodeQL databases, logs, and outputs are ignored by Git.
