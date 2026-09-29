# Architecture correction; not a refinement gain

The G04 patch edits arch/arm64/kernel/process.c. The legacy basename/similarity
object map returned arch/x86/kernel/process.o instead, and the evidence collector
used x86 include flags for the ARM64 source. Its saved error is asm/fpsimd.h not
found. The old0/0 execution and missing-native-record label therefore do not prove
anything about the intended ARM64 patch pair and are superseded, not counted as
negative method outcomes. Its completed no-internal refinement also used that
wrong oracle and needs a fresh correctly scoped comparison.

A whole39 input-path audit finds only this real mapping mismatch after normalizing
parent-directory components. AMD objects using amdgpu/../display or ../amdkfd
resolve to the actual requested source and are not mismatches. The other38cases
continue normally; their frozen files are not rewritten.

arm64_entry.py changes only architecture/object/build plumbing in an isolated
process: exact ARM64 source-to-object mapping, ARCH=arm64 configuration, scan
commands and compile-database preparation. It does not edit any checker. The
existing image supports AArch64. A dedicated detached Linux worktree isolates
configuration and output from every currently running x86 lane.

First establish the true original paired outcome without an LLM, retain actual
ARM64 reports and collect native CFG/call-graph records with ARM64 headers. Then
run native, no-internal and actual KNighter branches against those same corrected
inputs. Choose the objective from the real starting outcome, not the stale0/0.
If the original is already PDS, retain it as a normal zero-edit outcome rather than
claiming a refinement gain. No sample is dropped or hand-repaired.

New corrected inputs are stored separately with hashes; immutable old inputs and
their failed/superseded records remain. Existing paper availability/effect counts
must not be copied unchanged if this repair alters their measurement.
