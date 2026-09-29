# Independent review of sealed recorded-evidence export

Verdict: **proceed for the sealed recorded-evidence package and its completed lossless reconstruction.** Public-only byte verification and recorded-result reanalysis pass at R0/R1. The round-trip closure appended below supersedes the initial pending reconstruction status. This verdict does not authorize publication or certify the manuscript.

Reviewed packages: `release-evidence-20260929-v43-r3` and `release-distribution-20260929-v43-r3`. Checks were read-only; Python bytecode writes were disabled. Author remediation conclusions and other reviewer reports were not used. No model calls, analyzer scans, scientific experiments, source/trial changes, or sealed-package writes were performed.

## Export verification

The verifier copied inside the sealed export passed using its public root and **without** `--source-project`. It validated the full byte manifest, original/public hash crosswalk, selected inputs, canonical results, native evidence, baseline retention, normalized received-reply replay, and separate CodeQL records:

- 77,297 manifest-listed files, plus the manifest itself; 77,295 original/public crosswalk entries. Generated self-description containers are protected by the package manifest rather than recursive original hashes.
- 357 paired-record check occurrences and 162 refinement cells. These are verification counts, not independent scientific samples.
- 102 authoritative raw CSA origins, 511 logical report files, 218 object/side scans, 86 healthy logged zero scans, and three emitted baseline pairs.

The prior corrected G08/G28 closure and relative/absolute diagnostic-count issue are resolved. Selected source, checker/validation, initial/native trace, response-budget, and canonical aggregate dependencies are available through the exported virtual paths. No private-source fallback was requested.

## Committed and historical source closure

I independently compared committed Git blobs against the crosswalk's original hashes and enumerated archive paths. V22 contains exactly 403 committed blob paths and v43 exactly 433; all are bound at both `source-versions/` and the runnable `project/LLM-Native/` paths, with zero binding errors. V43 corresponds to the recorded selected revision `16b57931fb105e122897fb1e063e11c366e7137e`.

The v23 archive has exactly 403 committed blob paths and matches the actual `SemWeaver-example-v23` repository, as declared in `SOURCE_VERSION_STATUS.json`.

Historical v10 is explicitly marked dirty in two paths. Its committed archive matches HEAD. Both separately retained working-file snapshots and `tracked-working.diff` match their original hashes and status record, and differ from the committed versions as expected. The package does not pretend these working edits were committed. No reset or commit was performed for this review.

## Distribution integrity and readable entry points

The distribution byte verifier passes for 77,298 logical files. Independently, its logical path/size/hash index equals the export's complete manifest plus the manifest file. The 27,499 object sizes agree with object-backed rows; 148 shard descriptors partition those object identities exactly once. Compressed shards total 1,026,006,893 bytes. Logical evidence remains approximately 10.23 GiB; byte deduplication does not remove trial paths.

The export README, distribution unpack/verify instructions, and English prompt companion README are English and state the recorded-evidence and warning-metric boundaries. All 736 companion source bindings resolve through the exported crosswalk: 710 through `/artifact/project/` and 26 through `/artifact/work/`, with zero original/public hash errors. Historical raw language is retained and is not represented as an English experimental rerun.

The Linux COPYING/GPL and KNighter Apache notices are present and crosswalk-bound. The entry documentation explicitly grants no new project-level license.

Six whole-export pattern checks found zero matching files for known local owner identifiers, unvirtualized home-account paths, known credential shapes, private editor endpoints, literal bearer values, and URL-embedded authentication. No credentials or identifying owner URLs are reproduced in this review.

## Remaining limitations

1. R0/R1 closure applies to this exact exported/reconstructed payload. Fresh deterministic analyzer execution and end-to-end model execution were not performed; no R2/R3 result is claimed.
2. Integrity proves consistency of the exported recorded evidence and analysis, not trusted attestation of historical execution. Zero scans rely on healthy recorded receipts plus retained build/scan logs; absent raw stdout has not been invented.
3. This package does not independently adjudicate semantic target recall, deployment precision, unseen generalization, or causal native-evidence advantage. Main, repeated, focused-model, motivating, historical/superseded, and CodeQL records retain their actual scopes and chronology; no development/holdout labels are added.
4. Fresh analyzer/model execution requires pinned tools and external subject checkouts; byte-identical API replies are not promised. Privacy pattern checks are bounded checks, not proof of universal anonymity. Manuscript agreement, citations, complete legal/redistribution review, and final anonymous publication remain separate gates.

## Independent round-trip closure

Clean reconstruction is now complete at `release-reconstruction-20260929-v43-r3`. I independently compared the actual export and reconstructed manifests: bytes are exactly equal, SHA256 `c9ad6078fd4071994f9c56a84a70d07319593e4fcaa5ac2ec0170a1a763f60e5`. The reconstructed tree contains exactly 77,298 normal files and 10,979,140,347 bytes, with zero symlinks. Its actual logical paths/sizes and manifest hashes exactly match `DISTRIBUTION_INDEX.json`.

The reconstructed tree's own public verifier passes without source fallback. I additionally repeated it independently in image `sha256:2ddb870c297ffbb9b30b901f84da5542a8ae37eb8d6ce332eae1657fdd0014ec`, with network disabled, container root read-only, and only the reconstructed artifact mounted read-only. Exit status was zero: 357 paired-record occurrences, 162 refinement cells, and raw recount 102 origins/511 reports/218 object-side scans/86 healthy logged zeros/three baseline emitted pairs. These observed results agree with `FINAL_RECONSTRUCTION_RECEIPT.json`.

This closes exact compression reconstruction, R0 byte verification, and R1 recorded-output reanalysis. No sealed artifact was modified and no model or analyzer was executed. The limitations above remain in force.
