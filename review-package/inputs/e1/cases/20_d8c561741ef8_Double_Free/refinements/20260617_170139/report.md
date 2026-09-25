# E2 Final Sample 20 Refinement Report

- case_id: 20_d8c561741ef8_Double_Free
- commit_id: d8c561741ef83980114b3f7f95ffac54600f3f16
- analyzer: csa
- refinement_dir: `/anonymous/home/LLM-Native/research/knighter/e2/cases/20_d8c561741ef8_Double_Free/refinements/20260617_170139`
- checker: `/anonymous/home/LLM-Native/research/knighter/e2/cases/20_d8c561741ef8_Double_Free/csa/refinements/20260617_170139/csa/SAGenTestChecker.cpp`
- validation_dir: `/anonymous/home/LLM-Native/research/knighter/runs/knighter-v613/validation_20260617_172351`
- validation_log: `/anonymous/home/LLM-Native/research/knighter/runs/knighter-v613/validation_20260617_172351/knighter_validation.log`

## Strict Patch-Local Object Counts

- object: `drivers/net/ethernet/mellanox/mlx5/core/steering/hws/mlx5hws_send.o`
- baseline: buggy_alerts=2, fixed_alerts=1
- refined: buggy_alerts=1, fixed_alerts=0
- buggy_hit: true
- fixed_silent: true
- PDS: true

## Notes

Manual E2 refinement validation (csa): strict patch object buggy_alerts=1, fixed_alerts=0, buggy_hit=true, fixed_silent=true, PDS=true. Baseline was buggy_alerts=2, fixed_alerts=1. Manual repair applied to the latest automatic refine output after the automatic checker caused long-running Knighter validation and failed to produce a successful strict result; final checker models the failed SQ ready transition cleanup edge hws_send_ring_close_sq(sq) versus the fixed hws_send_ring_destroy_sq(mdev, sq).

Evidence was module-generated: baseline evidence records=12, post-validation records=19, missing=0. The strict metric is parsed from Knighter `scan-build ... make LLVM=1 ARCH=x86 <object>.o` results in `knighter_validation.log`, not HTML report counts and not v2 diagnostic rows.
