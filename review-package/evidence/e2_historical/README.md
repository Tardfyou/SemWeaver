# Historical E2 CSA/CodeQL records

These files are provided to make an older, mixed-provenance interface study
inspectable. They are **not** included in the paper's automatic 12-case
KNighter/SemWeaver effectiveness comparison.

## Navigation

- `tables/refine_results.csv` is the original 20-row result table (ten CSA
  and ten CodeQL detector-format rows). Its recorded baseline is 7 PDS and
  final state is 12 PDS. The old figure's 8-at-baseline value is not
  reproducible from this CSV.
- `runs/<sample>/primary/<backend>/<timestamp>/` contains available checker
  or query source (`.cpp` or `.ql`), evidence bundles, validation reports,
  and refinement versions. Paths containing `_manual` denote manual work or
  a manual baseline and cannot be counted as automatic refinement.
- `manifests/` and `datasets/curated/` provide available sample metadata and
  patches. Generator prompt design and complete per-row lineage are not
  recoverable from these historical records.

Seven of the twenty source paths contain manual markers; two rows record zero
model calls. The separate `../e2_legacy_audit_v1/RESULT.json` classifies these
rows and the unmarked candidate subset without promoting it to validated
automatic performance. The offline verifier checks the original CSV's SHA-256,
row count, marker counts, and presence of checker/query sources.

Reviewer-facing instructions are in English. Some immutable historical
reports, model exchanges, and source comments retain their original language
so that scientific records are not silently translated or reconstructed.
Binary CodeQL databases and BQRS caches are omitted; re-execution requires
the relevant public source revisions and CodeQL environment.
