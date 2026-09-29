# Independent artifact delta: raw-origin reconciliation

Verdict: **source raw-evidence closure passes; clean anonymous export remains pending.** This delta replaces the earlier report's raw-origin/report totals and its now-resolved inventory/verifier gaps. It does not certify full-package R0/R1.

Scope was restricted to current exporter/verifier code, `FINAL39_SUMMARY.json`, `AUXILIARY_SUMMARY.json`, corrected starting redirects, selected cell/paired receipts, direct raw validation files, and offline tests. No author remediation conclusions, other reviewer reports, scans, model calls, or scientific input changes were used.

I independently enumerated 264 selected entry occurrences: 156 starting/main portfolio entries, 36 focused-model entries, and 72 repeated-ablation arm/decode references. Repeated references are not additional independent samples. `starting_checker` selections resolve to their actual recorded starting receipt; copied starting receipts resolve through the corrected `validation_path`. All paths were normalized before origin deduplication.

| Scope | Distinct raw origins | Logical HTML reports |
| --- | ---: | ---: |
| All selected outputs, after corrected-starting redirects | 101 | 510 |
| All emitted baseline pairs | 3 | 3 |
| Emitted pairs already in selected outputs | 2 | 2 |
| Union checked by the verifier | 102 | 511 |

The additional emitted origin is `knighter/G14_7d42e097607c_Concurrency/paired-replay/attempt-2/RESULT.json`: warning counts `[1, 0]`, `common_adoption=false`. G14 attempt 1 and G30 attempt 2 are already selected. Including the unretained G14 output is appropriate baseline retention reconstruction, not an extra selected result.

The independently constructed union equals the verifier's redirected `self.raw_pairs` set exactly: **zero missing origins and zero unexpected origins**. Before redirecting copied starting receipts, `self.raw_pairs` has 113 path identities; normalization/redirecting produces 102 authoritative raw origins. Every selected output is covered.

My earlier 112-origin/644-report diagnostic contained a path-normalization error. It mixed relative corrected-starting paths with absolute selected-origin paths and counted 11 physical validations twice. They were G02, G03, G06, G08, G15, G17, G20, G24, G27, G31, and G36; their duplicated report totals were respectively 2, 4, 8, 4, 18, 22, 6, 54, 10, 4, and 2, summing to 134. Thus `112 − 11 = 101` and `644 − 134 = 510`; adding the one unretained baseline origin/report gives `102/511`. The difference was diagnostic double-counting, not distinct trial copies or omitted study outputs.

Current checks passed independently:

- Full source verifier: 357 paired-record occurrences, 162 cells, 102 raw origins, 511 report files, 218 object/side scans, 86 healthy logged zero scans, and three baseline emitted pairs.
- Corrected G08/G28 inventory: all 14 raw JSON/log/HTML files inspected are selected; no missing logical paths.
- The same offline verification/export/compression/alias/prompt test modules now pass 59 tests with `SEMWEAVER_REAL_EXPORT_AUDIT=1`.

Raw recount uses per-side/object `report-*.html`, requires retained scan/build logs and healthy receipts for zeros, excludes unattributed reports, and does not deduplicate equal bytes across sides/subjects. Baseline reconstruction now checks all emitted unique candidate pairs and retention.

The original source-level gap is closed. The remaining gate is to build the complete anonymous package, run its public verifier without private-source resolution, compress/unpack into a new directory, rerun verification from that reconstruction alone, and inspect the actual exported payload for privacy. No clean export was produced or certified by this delta review.
