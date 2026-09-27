# Usage

Run commands from the repository root.

`generate`, `evidence`, and `refine` initialize the configured LLM provider. Set the provider key in the shell or in a private `.env` file before running them.

## CLI Help

```bash
python3 -m src.main --help
```

## Local Component Check

Before a full run, verify optional local services and analyzer tools:

```bash
python3 -m src.main --help
${CLANGD:-/usr/lib/llvm-18/bin/clangd} --version
${CODEQL:-codeql} version
curl -fsS http://localhost:8001/api/v1/heartbeat || curl -fsS http://localhost:8001/api/v2/heartbeat
```

The ChromaDB check is only required when using RAG-backed knowledge search. `scripts/setup_rag.sh` starts ChromaDB and prepares `pretrained_models/`; the embedding model is downloaded on first RAG use if it is not already cached.

## Generate a Detector

```bash
python3 -m src.main \
  --config config/config.yaml \
  generate \
  --patch tests/tiny_buffer_lab/patches/tiny_copy_bounds_fix.patch \
  --output output/tiny_buffer_lab \
  --validate-path tests/tiny_buffer_lab \
  --analyzer csa
```

Analyzer options:

- `csa`
- `codeql`
- `both`
- `auto`

## Collect Evidence

```bash
python3 -m src.main \
  --config config/config.yaml \
  evidence \
  --patch tests/tiny_buffer_lab/patches/tiny_copy_bounds_fix.patch \
  --evidence-dir tests/tiny_buffer_lab \
  --output output/tiny_buffer_lab \
  --analyzer csa
```

## Refine an Existing Output

```bash
python3 -m src.main \
  --config config/config.yaml \
  refine \
  --input output/tiny_buffer_lab \
  --validate-path tests/tiny_buffer_lab \
  --evidence-input output/tiny_buffer_lab \
  --patch tests/tiny_buffer_lab/patches/tiny_copy_bounds_fix.patch \
  --analyzer csa
```

## Validate an Artifact

```bash
python3 -m src.main \
  validate \
  --checker output/tiny_buffer_lab/csa/ExampleChecker.so \
  --target tests/tiny_buffer_lab \
  --analyzer csa
```

CSA validation requires a compiled `.so` detector for full semantic checking. LSP validation requires `clangd`. CodeQL validation requires CodeQL CLI and a C/C++ database; SemWeaver can create a database for simple targets when `codeql_auto_create_db` is enabled.

## Smoke Fixture

See `tests/tiny_buffer_lab/MANUAL_TEST_STEPS.md` for a small end-to-end command sequence. This fixture is not an experiment dataset.

## Knighter Smoke Experiment

The Knighter smoke profile reads the CSV in `experiments/samples/` and stages
sample inputs from the external artifact package under
`artifacts/research/knighter/cases/`. Runtime files are written under
`artifacts/experiments/knighter/smoke/`.

```bash
./scripts/knighter_smoke.sh setup
./scripts/knighter_smoke.sh audit
```

To run the smoke experiment, configure the external Knighter runtime first:

```bash
export KNIGHTER_LLVM_DIR="$PWD/artifacts/external/llvm"
export KNIGHTER_LINUX_DIR="$PWD/artifacts/external/linux"
export KNIGHTER_HOST_DEPS_DIR="$PWD/artifacts/external/host_deps/jammy-amd64/root"
export OPENAI_API_KEY="<your key>"

./scripts/knighter_smoke.sh run
```

`run` executes the minimal generate smoke. `run-full` also enables the refine
step. Add future smoke cases by appending rows to
`experiments/samples/knighter/profiles/smoke.csv`.

## Sample Manifests

Sample manifest CSVs are under `experiments/samples/`. The full sample
environments are expected under `artifacts/` after unpacking the artifact
package. Keep runtime output roots under `artifacts/`:

```bash
python3 -m src.main experiment audit \
  --root artifacts/experiments/cross_backend \
  --manifest experiments/samples/cross_backend/profiles/samples.csv \
  --sample-id vul4c_cwe369_libtiff_cve20177595
```

The Knighter materialized case list is
`experiments/samples/knighter/profiles/knighter_materialized_cases.csv`; the smoke
wrapper remains the quickest end-to-end preflight.
