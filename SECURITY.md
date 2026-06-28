# Security Notes

This repository should not contain credentials, private endpoint URLs, local absolute paths, generated scan outputs, or author-identifying metadata.

Before publishing or archiving, run:

```bash
rg -n "sk-[A-Za-z0-9_-]{12,}|api[_-]?key|/home/|OPENAI_API_KEY=.*\\S|ANTHROPIC_API_KEY=.*\\S|DEEPSEEK_API_KEY=.*\\S" .
git status --short
```

Expected credential handling:

- Put real keys only in shell environment variables or a private `.env` file.
- Keep `.env` ignored.
- Keep `config/config.yaml` free of real secrets.
- Do not commit generated `output/`, `logs/`, `codeql_dbs/`, `codeql_cache/`, or `pretrained_models/`.

If a key was committed in any branch or history, revoke it before release.
