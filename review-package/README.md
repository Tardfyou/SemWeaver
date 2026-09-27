# SemWeaver: anonymous review artifact

This package supports inspection of automatic, patch-related refinement of generated static vulnerability checkers. It is supplied for review; no project-level reuse license is granted. Adapted KNighter code retains its upstream Apache-2.0 license and modification notices. Included Linux excerpts retain their upstream SPDX notices; see `licenses/` for Linux license material. These notices are not a new license for SemWeaver.

## Start here

1. Read `paper/main.pdf` and the exact reported values in `results/corrected/SUMMARY.csv`.
2. Run `python3 scripts/verify_corrected_review_package.py .` from this directory. It uses only the Python standard library, performs no network/model call, and does not execute a generated checker.
3. Inspect `results/corrected/RESULT.json` for every scored checker, validation reference, model-call budget, accepted candidate and repeated-subset result. Every packaged file is bound by `ARTIFACT_MANIFEST.json`.

The verifier checks file hashes, redaction crosswalks, original inputs, all native-evidence bindings, corrected metric arithmetic, scored paired executions, candidate identities, model-call ceilings, repeated-subset denominators, the motivating example and the CodeQL cost appendix. It rejects a scored execution accompanied by scan-build failure artifacts or an uncaught agent exception in its retained refinement chain.

To independently recompute the tables without model calls, use `python3 scripts/reaggregate_scanfix.py --data-root evidence/fse_revision --redactions REDACTIONS.json --output-dir /tmp/semweaver-recomputed-unique`. Choose a fresh output directory. The old portfolio files serve only as frozen source-batch indexes; their old totals are never copied as the corrected result.

## Study scope and denominators

- The input inventory contains 286 generated records; 241 unrefined records span 39 commits. A frozen, non-random upstream-score rule chooses one automatically generated checker per commit. All 39 selected subjects remain in the study.
- Independent corrected starting replay yields **15 subjects warning-positive on both revisions and 24 silent vulnerable revisions**. These are operational warning strata, not 15 independently adjudicated target detections. The first group supports the direct fixed-side-noise comparison; on the second, KNighter's no-report branch performs no model edit. The full 39-case result is a system comparison, not a claim that KNighter attempted all recovery edits.
- The actual KNighter loop and the direct SemWeaver comparison share starting checkers, GPT-6-Luna high, the first five frozen reports (or all available when fewer), and per-subject model-call ceilings derived from KNighter's actual calls. Call matching is not token matching.
- Strict adoption requires an execution-valid vulnerable-revision warning signal and fixed-side silence. Otherwise the scored portfolio retains the independently executed starting checker. Invalid candidate attempts are not false-negative measurements. Valid warning-count reductions are reported separately from adopted PDS and F1 totals; they do not automatically imply correct target detection.
- Precision, recall and F1 count **78 paired patch-version decisions**, not independent warning sites or deployment precision. Fixed-object warning counts are a separate burden measure.
- The repeated subset retains the same 12 subjects, now correctly stratified as **5 fixed-noisy and 7 initially silent cases**. Repeated decodes are not additional independent subjects.
- E3 allows eight calls per subject. E4 full-cohort runs and every GPT repeat allow two. Additional GLM repeats allow two calls for fixed-noisy subjects and eight for recovery; their first decode reuses the two-call full-cohort result. Repeated model outcomes are therefore configuration-specific, not a budget-matched model ranking.

The full-cohort native/no-internal comparison and all model results are reported as observed, including ties and failures. This package does not establish universal native-evidence superiority, unseen-project generalization, or whole-kernel throughput.

`docs/TARGET_ALIGNMENT_NOTES.md` checks diagnostic locations against source fixes and records G08 as a counterexample to interpreting any vulnerable-side warning as a true target hit. The five newly warning-positive GPT cases have source/diagnostic correspondence to their patch mechanisms; this qualitative inspection is not exhaustive per-warning adjudication or a replacement for the version-level metric definition.

## Layout

- `source/SemWeaver/`: final corrected implementation. `source/frozen-implementations/` preserves implementation versions used by retained runs. Git history is omitted; revisions are recorded in the manifest.
- `source/manuscript/` and `paper/`: final LaTeX sources and compiled paper.
- `inputs/cases/`: frozen generated checker, patch, metadata and plan per subject. `inputs/generate_only_v1/` retains selection provenance.
- `evidence/generate_only_evidence_all39_v8_r5/` and related evidence paths: typed bundles and raw Clang dumps. The 192 records comprise 80 strictly analyzer-internal, 51 analyzer-output and 61 source-derived records; all 80 internal records have raw-output/source-scope bindings. Availability is 38/39 subjects.
- `evidence/fse_revision/`: original and corrected run records, English prompts and rendered exchanges where available, generated candidates, build/paired logs, correction queues and audits. **Historical raw runs are not the authoritative scoring table.**
- `results/corrected/`: the only current effectiveness aggregation. The `correction_map` binds superseded scan results to fresh replays. Provider-correction queues identify replacements for interrupted cells.
- `evidence/motivating_example/`: the G21 capacity-aware example, model-edit lineage, original Linux pair, all diagnostic suites and intermediate failures, editable SVG/PDF/PNG, and redraw instructions.
- `evidence/e2_codeql/`: one separate automatic CodeQL development case with three decodes, not a KNighter/CodeQL baseline comparison.
- `evidence/backend_cost/`: backend integration interfaces, an explicit code-footprint inventory, measured query/setup/model costs, and a separately labelled Codex-assisted basic-adaptation estimate. Developer-hours were not measured.

## Why superseded records remain

The correction audit identified scan-build argument forwarding failures, checker crashes or unresolved symbols that some old result files had misclassified as valid zero-warning executions. Corrected tool versions preserve LLVM option operands, inspect raw failure artifacts, probe plugin loading and reject uncaught agent/SDK exceptions. The original files remain unchanged for audit; their old `execution_valid` flags are **not** used as authority for the corrected results.

G08's actual starting behavior is 2/2 vulnerable/fixed warnings, not the erroneous 0/0. It was reclassified without dropping or replacing the subject, and its baseline, matched comparison, ablation and model cells were rerun accordingly. Other affected cells were selected by execution/transport errors, not by unfavorable effectiveness. A frozen prefix is reusable only before divergent feedback; acceptance uses the first correctly validated PDS, not a favorable later candidate. Actual historical calls and retained-prefix calls remain distinct.

## Reproduction requirements

The measured CSA environment uses Ubuntu Clang/LLVM **18.1.3**, Python **3.12.3**, and `scan-build`. Each paired run checks out the recorded Linux patch commit and parent and rebuilds patch-related objects with `allyesconfig`; it is not a scan of one common release snapshot. The CodeQL replay uses **2.26.4**, `codeql/cpp-all` **8.0.3**, and no-build databases.

`environment/clang18.Dockerfile` is the analyzer image recipe. `environment/measured/RUNTIME.json` records the actual OS/Python package versions, and `python-requirements.txt` records the 77 Python dependencies. The recipe uses distro repositories: compare installed versions with the recorded inventory rather than assuming a future package resolution is identical. The original execution mounted a Python 3.12 virtual environment into this image; a fresh setup must also provide that environment (install Python venv/pip tooling if needed).

Use a **dedicated, disposable Linux checkout**, not a development tree with uncommitted work: the paired validator runs `make clean` and checks out recorded revisions. The entry point is `source/SemWeaver/experiments/knighter/experiment/validate_frozen_csa_candidate.py`; supply a frozen input or generated candidate, its `inputs/cases/<case_id>` directory, the Linux checkout, a new output directory and a new backend workspace. For model recovery runs, `inputs/corrected-starting/<case_id>/RESULT.json` supplies corrected starting feedback. Exact run settings, report identities and budgets are retained in each batch manifest.

Full reruns need the public source revisions, recorded toolchain and model access. Private credentials, Linux worktrees, CodeQL databases, compiler caches, shared-library binaries and virtual environments are not bundled. Model names are aliases, not immutable weight snapshots; stochastic outputs need not match byte-for-byte. Use fresh output directories rather than overwriting retained records. Offline verification does not require live model endpoints.

All explanatory documentation is English. Raw model/tool traces are retained as emitted and can include original-language diagnostics. The methods section describes prompt inputs and deterministic versus model-based steps; exact exchanges, rather than reconstructed prompts, are the reproducibility authority. Missing historical generation traces are not invented.

Machine/account paths and private gateway addresses are anonymized. `REDACTIONS.json` preserves original and packaged SHA-256 values so reviewers can follow scientific bindings without recovering private machine identities.
