# E2 Refinement Report: 09_df94cb2e57b2_Integer_Overflow

- status: manual_validated
- baseline: buggy_alerts=6, fixed_alerts=5
- refined: buggy_alerts=2, fixed_alerts=0
- validation_dir: research/knighter/runs/knighter-v613/validation_20260617_145524
- summary: Manual E2 refinement validation (csa): buggy_alerts=2, fixed_alerts=0, buggy_hit=true, fixed_silent=true, PDS=true. Baseline was buggy_alerts=6, fixed_alerts=5. Manual checker models the patch semantic edges: disk_res_sectors must be u64 in fs usage accounting, and bch2_extent_fallocate sectors must be u64 before min_t(u64, ...) and accounting sinks.
