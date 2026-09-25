# FSE Replication Package Contract

The FSE package is evidence-complete rather than source-only. It must contain
every input and output used by a paper numerator, denominator, table, or figure.

## Required layout

```text
artifact/
├── ARTIFACT_MANIFEST.json
├── environment/
│   ├── container-images.txt
│   ├── tool-versions.json
│   └── hardware.json
├── cohorts/
│   ├── e1_frozen.csv
│   ├── e2_frozen.csv
│   └── exclusions.csv
├── prompts/
│   ├── current_en/
│   └── historical_exact/
├── routing/
│   ├── annotations/
│   ├── gold.csv
│   ├── predictions.csv
│   └── router_metrics.json
├── runs/
│   └── <run-id>/
│       ├── RUN_MANIFEST.json
│       ├── input_detector/
│       ├── patch/
│       ├── evidence/
│       ├── prompts_and_raw_responses/
│       ├── candidates/
│       ├── validation/
│       └── final/
├── robustness/
│   └── <case-id>/
│       ├── manifest.csv
│       ├── fixtures/
│       ├── raw_logs/
│       └── metamorphic_report.json
├── derived/
│   ├── tables/
│   └── figures/
└── audits/
    ├── evidence_provenance.json
    ├── result_integrity.json
    └── artifact_consistency.json
```

## Run manifest

Every run manifest records:

- immutable run id, cohort id, case id, analyzer, method, and repetition;
- hashes for the patch, source revision, starting detector, prompt files,
  configuration, evidence bundle, and transformation manifest;
- exact model identifier, endpoint class, sampling settings, request ordering,
  iteration/token/time limits, and observed usage;
- execution health independent of scientific outcome;
- one intervention label: `automatic`, `human_assisted`, `environment_error`,
  or `excluded`;
- PDS components, deterministic review, evidence-origin eligibility,
  metamorphic outcomes, and RRS;
- paths to raw prompts/responses, candidates, compile/query logs, and validation
  diagnostics.

## Required verification

Before packaging:

```bash
python3 experiments/audit_evidence_provenance.py artifact/runs \
  --output-json artifact/audits/evidence_provenance.json \
  --output-csv artifact/audits/evidence_provenance.csv

python3 experiments/audit_result_integrity.py \
  artifact/derived/tables/automatic_results.csv \
  --require-automatic \
  --output-json artifact/audits/result_integrity.json

python3 experiments/routing/score_router.py \
  --gold artifact/routing/gold.csv \
  --predictions artifact/routing/predictions.csv \
  --output artifact/routing/router_metrics.json
```

The table/figure regeneration command must read only frozen manifests and raw
records. It must fail on missing files, hash mismatch, duplicate run ids,
unlabelled intervention, or denominator drift.

## Security and anonymity

- Never package `API_KEYS.txt`, `.env`, credential helpers, endpoint secrets,
  absolute home paths, author names, editor metadata, or Git history.
- Scan both file contents and archive member names before upload.
- Rotate any credential that has entered a chat transcript, shell history, or
  prior archive.
- Use an anonymous immutable review URL and verify extraction in a clean
  directory before submission.

## Result boundary

Human-assisted repairs remain available for diagnosis but are stored outside the
automatic result table. Source-derived fallbacks, diagnostics, and
analyzer-internal facts retain separate origin counts. No fallback is promoted
to the analyzer-internal treatment, and no missing outcome is zero-filled.
