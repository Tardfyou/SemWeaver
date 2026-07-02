# Configuration

Main configuration lives in `config/config.yaml`. The checked-in file uses environment variables and contains no credentials.

## LLM Provider

Supported provider names:

- `openai`
- `deepseek`
- `anthropic`
- `openai_compatible`

Example:

```bash
export SEMWEEVER_LLM_PROVIDER=openai
export SEMWEEVER_MODEL=gpt-4.1
export OPENAI_API_KEY="<your key>"
```

For OpenAI-compatible endpoints:

```bash
export SEMWEEVER_LLM_PROVIDER=openai_compatible
export SEMWEEVER_MODEL="<model name>"
export OPENAI_API_KEY="<your key>"
export OPENAI_BASE_URL="https://example.com/v1"
```

Do not commit `.env` files or real API keys. `.env.example` documents the expected variables. SemWeaver automatically loads `.env` from the repository root before expanding `config/config.yaml`.

## Tool Paths

Common overrides:

```bash
export LLVM_DIR=/usr/lib/llvm-18
export CLANGXX=/usr/lib/llvm-18/bin/clang++
export CLANGD=/usr/lib/llvm-18/bin/clangd
export CODEQL=codeql
export CHROMA_HOST=localhost
```

`compilation.llvm_dir` and `compilation.clang_path` drive CSA checker compilation. `lsp.clangd_path` drives LSP validation and defaults to `CLANGD` or `/usr/lib/llvm-18/bin/clangd`. Keep these paths on the same LLVM major version when possible.

Minimal path check:

```bash
test -x "${CLANGXX:-/usr/lib/llvm-18/bin/clang++}"
test -x "${CLANGD:-/usr/lib/llvm-18/bin/clangd}"
${CODEQL:-codeql} version
```

## ChromaDB and Embeddings

RAG-backed knowledge search is configured under `knowledge_base`:

- `knowledge_base.chromadb.host` defaults to `CHROMA_HOST` or `localhost`.
- `knowledge_base.chromadb.port` defaults to `8001`.
- `knowledge_base.embedding.model` defaults to `sentence-transformers--all-MiniLM-L6-v2`.
- `knowledge_base.embedding.cache_dir` defaults to `pretrained_models/`.

Use `bash scripts/setup_rag.sh` to install RAG dependencies, start ChromaDB, and create the embedding cache directory. The model is downloaded on first use unless the cache already contains it.

## Output Locations

By default, runtime artifacts are written under:

- `output/`
- `logs/`
- `artifacts/experiments/`
- `artifacts/external/`
- `codeql_dbs/`
- `codeql_cache/`
- `pretrained_models/`

These directories are ignored and should not be included in a source release.

## Optional KNighter E2 Integration

The KNighter/E2 validation path is disabled by default. Enable it only after
placing the required external environment under `artifacts/` or equivalent
local paths.

Example configuration override:

```yaml
validation:
  semantic:
    knighter_e2:
      enabled: true
      knighter_root: "${PROJECT_ROOT:-.}/experiments/knighter/baseline"
      llvm_dir: "${PROJECT_ROOT:-.}/artifacts/external/llvm"
      linux_dir: "${PROJECT_ROOT:-.}/artifacts/external/linux"
      host_deps_dir: "${PROJECT_ROOT:-.}/artifacts/external/host_deps/jammy-amd64/root"
      result_dir: "${PROJECT_ROOT:-.}/artifacts/experiments/knighter/runs"
```
