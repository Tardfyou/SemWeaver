# KNighter Baseline Source Subset

This is the source subset required by the baseline integration scripts. It
contains Python drivers, prompt templates, LLVM plugin helper files, Docker
setup files, and dependency declarations.

## Setup

```bash
cd experiments/knighter/baseline
python3 -m venv .venv
source .venv/bin/activate
python3 -m pip install -r requirements.txt
cp src/llm_keys_example.yaml src/llm_keys.yaml
cp config-example.yaml config.yaml
```

Edit `config.yaml` so `LLVM_dir`, `linux_dir`, and `result_dir` point to paths
under `artifacts/`.

For Docker Compose, keep host-side external assets under the repository
`artifacts/` directory. The compose file mounts that directory at `/artifacts`
inside the container, so container-side config values should use paths such as:

```yaml
LLVM_dir: "/artifacts/external/llvm"
linux_dir: "/artifacts/external/linux"
result_dir: "/data/results"
key_file: "/app/src/llm_keys.yaml"
```

Then run:

```bash
docker compose run --rm knighter
```

## Notes

The full third-party parser trees, generated checker database embeddings,
commit lists, logs, and result outputs are not included in this source release.
They are either rebuilt by setup scripts or supplied through the separate
artifact data package.

If the optional checker example database is absent, the baseline runs without
sampled checker examples. The source subset includes a lightweight parser
fallback, so it does not require vendoring the original tree-sitter submodule.
