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
export CODEQL=codeql
export CHROMA_HOST=localhost
```

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
