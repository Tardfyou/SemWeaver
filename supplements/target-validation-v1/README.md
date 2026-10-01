# Target-validation supplement

This supplement accompanies SemWeaver's frozen main artifact. It adds source/diagnostic audits, controlled source-property fixtures, twelve selected-clue decisions, a separate compiler-completion phase, and secondary source/path interpretations. It does not replace the original 39-subject comparison or claim an improved method.

## Quick start: no API key or analyzer required

Download the anonymous repository ZIP and unpack it locally. From its root:

```sh
python3 supplements/target-validation-v1/restore_review.py verify supplements/target-validation-v1
python3 supplements/target-validation-v1/restore_review.py restore supplements/target-validation-v1 --output ../target-validation-exact
python3 -B ../target-validation-exact/scripts/verify_supplement.py --root ../target-validation-exact
python3 -B ../target-validation-exact/scripts/reanalyze_supplement.py --root ../target-validation-exact --output ../target-validation-r1
```

Python 3.9+ and its standard library are sufficient. Use a fresh output directory. Allow approximately 2 GB of temporary disk space. R0 checks public-file bytes and membership. R1 reads recorded PLISTs and frozen annotations to reconstruct the main direction join, source-audit counts, primary fixture table, separate validation-phase vectors, compiler-continuation accounting and semantic-review coverage. It makes no model, compiler, analyzer or network call. This is recorded-output reanalysis, not an end-to-end rerun or independent semantic judgment.

## Where to look

- `METHODS.md`: selection, timing, phase accounting, limitations and exclusions.
- `CLAIM_EVIDENCE_INDEX.json`: claim-to-file navigation.
- `evidence/study/combined-evidence/`: complete 11-non-tie audit joined to all 39 original directions.
- `evidence/study/fixtures-draft/`: 48 source-property fixtures frozen before checker scans.
- `evidence/study/intervention-inputs-v3/`: actual full/withheld inputs.
- `evidence/study/intervention-runs/`: original received decisions, including strict-invalid outputs.
- `evidence/study/compiler-repair-validation-v1/`: separate terminal vectors and raw reports.
- `evidence/study/intervention-semantic-review-r1/`: condition-masked packet, judgments, reconciliation and public phase bindings.
- `evidence/dependencies/`: exact source, HTML, log and checker dependencies from the earlier study.

## Provenance and reading precautions

`EXPORT_MANIFEST.json` distinguishes immutable-original hashes from public-derivative hashes. Workstation names and private gateway addresses are anonymized. Embedded legacy hashes continue to identify originals; the export manifest verifies the delivered bytes. `/f/` analyzer paths, source lines, conditions, code identities and recorded measurements are preserved. Path aliases are evidence-navigation aids, not additional trials. Historical scripts and commands are retained for provenance; only the two `scripts/` entrypoints above are portable supported R0/R1 commands.

Some original prompts and received text contain non-English fragments. They remain experimental evidence and have not been rewritten or translated as if they were the original input. Public release documentation is English.

No project-wide license is newly granted. Materials are supplied for scholarly review. Existing third-party copyright, SPDX and license notices retain their own terms. See `NOTICES.md`.
