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
- `codeql_dbs/`
- `codeql_cache/`
- `pretrained_models/`

These directories are ignored and should not be included in a source release.
