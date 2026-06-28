# Usage

Run commands from the repository root.

## CLI Help

```bash
python3 -m src.main --help
```

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
  --checker output/tiny_buffer_lab/csa/ExampleChecker.cpp \
  --target tests/tiny_buffer_lab \
  --analyzer csa
```

CSA validation requires a compiled `.so` detector for full semantic checking. CodeQL validation requires CodeQL CLI and a C/C++ database; SemWeaver can create a database for simple targets when `codeql_auto_create_db` is enabled.

## Smoke Fixture

See `tests/tiny_buffer_lab/MANUAL_TEST_STEPS.md` for a small end-to-end command sequence. This fixture is not an experiment dataset.
