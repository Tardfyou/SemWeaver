# KNighter Baseline Integration

This directory contains source needed to run KNighter-style baseline and E2
checks used by the SemWeaver artifact. It is anonymized and source-only.

Large external components are intentionally excluded:

- Linux kernel checkout.
- LLVM/Clang checkout and build tree.
- Host dependency sysroot.
- Generated baseline outputs, scan reports, and logs.
- API keys and local model credentials.

Place those runtime assets under `artifacts/`, for example:

```text
artifacts/external/linux
artifacts/external/llvm
artifacts/external/host_deps/jammy-amd64/root
artifacts/experiments/knighter
```

The default SemWeaver configuration keeps `knighter_e2` disabled. Enable it
only when the external environment is populated.
