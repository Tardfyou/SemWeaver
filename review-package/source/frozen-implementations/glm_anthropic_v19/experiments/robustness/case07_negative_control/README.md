# Case 07 Patch-Binding Negative Control

This fixture suite tests the legacy `1/0` case-07 checker that was used in the
previous motivating example. It is a negative control, not a SemWeaver result.

The historical candidate hard-codes the enclosing function, helper, return
variable, cleanup label, and statement adjacency. The suite retains the original
vulnerable/fixed pair and adds three bug-preserving transformations plus one safe
near-miss that retains the original surface names.

Expected behavior for a reusable ownership checker is recorded in
`manifest.csv`. A patch-bound checker is expected to miss at least the renamed
function, inserted-statement, and renamed-label variants, and may warn on the
owned-before-failure near-miss.

The fixtures deliberately avoid claiming Linux-level representativeness. They
are a focused construct test showing that PDS alone can accept an exact patch
fingerprint. Final RRS results must use the frozen per-subject transformation
sets defined after each candidate checker is frozen.

After compiling the checker as a Clang 18 plugin, run:

```bash
python3 ../run_csa_fixture_gate.py \
  --manifest manifest.csv \
  --plugin /path/to/SAGenTestPlugin.so \
  --output-dir /path/to/audit-output
```

The runner hashes the manifest, plugin, and every fixture; preserves raw logs;
and exits non-zero if any expected decision fails. For this negative control, a
non-zero exit is the expected demonstration that historical PDS did not imply
robustness.

`observed_legacy_checker_result.json` records the completed Clang 18 negative
control. All six fixtures analyzed, but the legacy checker passed only 1/4
positive variants and 1/2 negative variants (`robust=false`). The compact result
is checked in; the runner regenerates the full report and raw diagnostic logs.
