# Static target-alignment checks and metric boundary

This is a qualitative source/diagnostic inspection, not a new scoring oracle, a dynamic exploit test, or exhaustive per-warning adjudication. The reported PDS and F1 remain **patch-version warning-discrimination metrics**. A warning-positive vulnerable revision does not, by itself, prove that the designated vulnerability was detected. No checker was manually edited during this inspection.

## Counterexample: G08

The correctly executed original G08 checker emits two warnings on each revision. Its warnings at `drivers/net/ethernet/marvell/octeontx2/nic/rep.c:652–653` concern the `if (!ndev)` allocation-failure branch. There is no newly allocated current `net_device` on that branch; the exit loop releases earlier iterations' devices. The patch instead inserts `free_netdev(ndev)` on failure of `rvu_rep_devlink_port_register`, around the old lines 682–684.

Some model-produced G08 candidates change 2/2 warnings to 1/1 but retain the warning at line 653. This is a warning-count reduction, **not evidence of retaining or recovering the target leak**. It is not PDS and is not adopted into strict portfolio gains. The 15-case group must therefore be called “warning-positive on both revisions” or operationally “fixed-noisy,” not 15 independently confirmed target detections.

## Five newly warning-positive GPT cases

For the five cases changing from an initially silent vulnerable revision to adopted PDS in the completed GPT full-cohort component, the retained diagnostic locations/messages are consistent with the source fix as follows. This supports a target-alignment interpretation for these examples without converting the entire study into per-site detection precision/recall.

| Case | Retained vulnerable-side anchor | Source relation to the patch |
| --- | --- | --- |
| G05 | `fs/fhandle.c:73`, `copy_to_user` from the allocated handle | The patch changes the handle allocation from `kmalloc` to `kzalloc`; the warning concerns copying potentially uninitialized bytes from that buffer. A duplicated helper-header warning is not an extra vulnerability. |
| G18 | `io_uring/net.c:562,565`, signed narrowing before `check_add_overflow` | These are the two cast-bearing overflow-check expressions changed by the patch. Duplicate emitted diagnostics are not independent sites. |
| G19 | `ice_common.c:1013` and other cleanup-related exits | `pcaps`/`mac_buf` have `__free(kfree)` attributes without initialization; early return can invoke cleanup before assignment. The patch initializes them to NULL and similarly adjusts other declarations. The run also emits header/other-function warnings; those additional reports are not all adjudicated as true positives. |
| G28 | `bnxt_ulp.c:211`, missing `hwrm_req_drop` before return | The patch redirects this error return to the common drop label. |
| G38 | `drivers/net/macsec.c:3802`, direct `metadata_dst_free` | The patch replaces the direct free with reference-counted `dst_release`; the diagnostic identifies that operation. |

## Other adopted case IDs inspected

| Case | Diagnostic/source correspondence | Boundary |
| --- | --- | --- |
| G01 | Array accesses in `dml2_wrapper.c:79,81`; the patch introduces the missing index guard | Source-level bounds relation, not broad loop/alias coverage. |
| G02 | Replay decision in `smb2ops.c:1317` after cleanup; the patch moves `ea = NULL` into the replayed initialization region | The warning concerns a freed pointer reused across retries; no independent whole-program ownership proof is supplied. |
| G09 | `kfree(mt->fc)` in `mlx5hws_definer.c:1948`; the patch reroutes the earlier error edge to skip that free | Patch-local cleanup-path correspondence; no claim that the detector generalizes across allocators. |
| G12 | `memcontrol.c:725`, the shared per-CPU update replaced by READ_ONCE/WRITE_ONCE operations | These wrappers do not make an entire read-modify-write transaction atomic; the diagnostic is related to the patched access discipline. |
| G13 | Overflow check following `roundup_pow_of_two` in `stackmap.c:96` | The source patch explicitly concerns 32-bit overflow. The x86-64 analysis run is not a 32-bit runtime-vulnerability witness. |
| G14 | End of the reset worker in `adf_aer.c:154`; the patch changes timeout/completion-dependent cleanup | Source-level worker/submitter correspondence, not verified interprocedural path feasibility. |
| G17 | Shared-state read at `kernel/workqueue.c:4188`, moved under `from_cancel` by the patch | This is the extra PDS case in the direct native/no-internal contrast, but that single contrast does not establish a stable causal advantage. |
| G21 | Look-ahead access in `link_dp_dpia_bw.c:208`; loop bound narrowed by one | The separately developed motivating checker additionally has finite metamorphic diagnostics; those are not held-out population evidence. |
| G23 | HTML reports at `emac.c:740–742` for using private netdev data after `free_netdev` | The patch moves `free_netdev` after those uses. Console progress logs suppress warnings after the first ten, so HTML reports—not a truncated console excerpt—are the location authority. |
| G26 | Subtraction at `fs-io-direct.c:91`; the patch guards the derived `shorten` value | Underflow-related source correspondence. |
| G30 | `vg_clk_mgr.c:550`, indexing a seven-entry array under an eight-step loop | The patch adds the missing bound guard; this also covers the accepted KNighter case. |

## Required writing constraints

- Say “warning-positive vulnerable revisions” when describing the 20 versus 15 version-level count; do not call this 20 versus 15 independently confirmed vulnerabilities.
- Describe preservation as preservation of the observed vulnerable-version warning signal under the paired gate. The static checks above provide additional case-level context, not a theorem about target fidelity.
- Do not describe G08's 2/2 to 1/1 as a target-preserving detector improvement. It is only a burden diagnostic, and neither strict PDS nor a proven target detection.
- Preserve the distinction between fixed-side warning burden and independently adjudicated false-positive precision. Full per-site labeling and unseen-code generalization remain outside the study.
