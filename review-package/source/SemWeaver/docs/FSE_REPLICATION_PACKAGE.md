# FSE replication package

The final review package wraps this source tree. Its root README and
`results/corrected/RESULT.json` are the authoritative protocol and numerical
results. Historical protocol documents in this source tree describe development
stages; their example cohort sizes and result fields are not current findings.

## Final layout

- `source/SemWeaver/`: corrected implementation and English prompt templates.
- `source/frozen-implementations/`: historical implementations used by retained runs.
- `source/manuscript/`, `paper/main.pdf`: manuscript source and compiled paper.
- `inputs/`: all 39 frozen generated checkers, patches, metadata and selection provenance.
- `evidence/`: native dumps, origin bindings, raw model/tool exchanges, candidate source, paired logs, corrections, example diagnostics and CodeQL cost records.
- `results/corrected/`: only the authoritative version-level result aggregation.
- `environment/`: analyzer recipe and measured tool/dependency inventory.
- `scripts/`: offline verification and deterministic reaggregation.
- `ARTIFACT_MANIFEST.json` and `REDACTIONS.json`: file integrity and anonymization crosswalks.

## Offline verification

From the package root:

~~~bash
python3 scripts/verify_corrected_review_package.py .
python3 scripts/reaggregate_scanfix.py --data-root evidence/fse_revision \
  --redactions REDACTIONS.json --output-dir /tmp/semweaver-recomputed-unique
~~~

Use a fresh output directory. Neither command calls a model or executes a
generated checker. Full paired reruns require a dedicated disposable Linux
checkout, the measured toolchain and the frozen input/candidate.

## Interpretation and provenance

PDS means a warning-positive vulnerable revision and silent fixed revision,
within patch-related build objects. It does not independently establish correct
target localization. F1 describes paired patch-version decisions, not deployment
precision or target-vulnerability recall. Fixed-side warning counts measure
inspection burden.

The 39 starting subjects are retained throughout: 15 warn on both revisions and
24 are initially silent. The same 12 repeated subjects split 5/7. KNighter's actual
no-report branch makes no model edit, so the 15-case matched task and broader
39-case system comparison answer different questions. The full-cohort native
evidence ablation ties; universal causal benefit is not established.

Raw superseded executions remain for audit but are not authoritative. Corrected
hash-bound references and replacement queues define scored outcomes; invalid
execution or uncaught SDK exceptions are never successful zero-warning scans.
Source-derived fallbacks, analyzer outputs and analyzer-internal records keep
separate origins. Exact historical traces are not reconstructed if missing.

No project-level license is added. Upstream licenses and notices remain in force.
Credentials, identifying machine paths, source Git history, tool caches,
databases, model caches and full Linux worktrees are excluded.
