# Independent final venue and claim review

Verdict: **REVISE**. The inspected draft supports a bounded, auditable checker-editing study with mixed warning-level outcomes. It does not establish general vulnerability-detection improvement or a dependable advantage from internal evidence. This is a scientific and submission checkpoint, not an acceptance prediction.

Reviewed independently on 2026-09-29, approximately 17:54–18:00 CST. I read the requested manuscript and raw summaries, including motivating replay results; the motivating decision document was used only for lineage. Author review conclusions and other reviewer reports were excluded. No model calls, experiments, or manuscript edits were performed.

## Strongest criticism

The principal FSE risk is the gap between the proposed provenance-guided contribution and the demonstrated utility. In `FINAL39_SUMMARY.json`, independently summing `portfolios.*.rows` reproduces native/control/KNighter TP = 23/21/15, FP = 18/13/13, fixed reports = 123/68/69, replies = 150/133/179 and F1 = .5750/.57534/.44776. Native improves warning coverage against an actual KNighter loop that makes no recovery edit on 24 silent subjects; that is a workflow-policy advantage, not evidence that KNighter attempted those edits unsuccessfully. Native essentially ties control's F1, has fewer PDS cases (5 versus 8), and substantially more fixed reports. The 15-case reduction is concentrated in G24 (native 2/2 versus control and KNighter 27/27), whose target preservation remains unverified. G12 contributes 73 fixed reports. These facts sharply limit any interpretation of semantic correctness or universally useful suppression.

`AUXILIARY_SUMMARY.json/repeated_ablation` contains 36 complete paired decodes on 12 subjects, 28 ties/other, five favorable and three unfavorable comparisons; five subjects always tie and seven have mixed outcomes. No subject benefits across all three. `SELECTION.json/comparison_scores` records two configurations on six overlapping subjects, with six reused pairs. This is disclosed exploratory overlap, not an invented independent development/holdout split. The novelty/importance burden remains: explain the engineering insight obtained from these mixed outcomes, rather than treating provenance integrity itself as demonstrated efficacy. Current abstract, results and conclusion mostly do this honestly.

## Required claim revisions

- `sections/02-background.tex:23–27`: “seldom semantically adequate” asserts unmeasured prevalence; “cannot tell” and “must guess” overstate what feedback plus patch/source reasoning permits. Use conditional language consistent with the introduction.
- `sections/02-background.tex:39`: “correctness gate” should mean operational warning-retention gate. Equation Accept preserves nonzero warning presence, not target location or every vulnerable warning.
- PDF Figure 2, page 6, still labels its stage “Target-preserving refinement,” contradicting that boundary. Replace the label. This is a claim correction, not visual polishing.
- Figure 1's caption should explicitly identify the separately budgeted 14-response illustrative sequence; the surrounding prose does, but the standalone caption does not. Replay identities `648d83…` and `5e953d…` respectively pass 74/114 and 114/114 fixtures. Preserve their separation, fixture-informed refinement, and finite-test limits.
- `sections/06-discussion.tex:2` should qualify noise reduction by stratum; the whole-cohort native burden increases. The “afternoon” adaptation estimate (`03-approach.tex:150`) is labelled unmeasured but remains unsupported and expendable.

## Checks that pass within scope

Static/dynamic definitions are separated correctly: `INITIAL_EVIDENCE_CATALOG.json` has 193 records = 81 internal + 51 outputs + 61 patch-derived records on 39 subjects; its state/lifetime type names do not establish symbolic-state proofs. `NATIVE_EVIDENCE_AVAILABILITY_STATUS.json` independently uses 150 attempts: 103 usable, 11 compile unavailable, 18 unsupported, 13 zero callbacks, one crash, four capped. Missing observations are not safety evidence.

The manuscript preserves E3/E4 subset denominators, actual replies, natural eight-ceiling KNighter reuses, stopping policies, invalid attempts, candidate crashes, sampling uncertainty, native-only guard asymmetry, and configuration overlap. E4 is 12 subjects × three configurations, one decode each, with unequal actual computation. CodeQL remains a separate single automatic case, including the unfavorable 22/22 third decode; historical mixed/manual results are excluded. No broad backend-transfer conclusion follows.

## Venue and remaining checks

The [official FSE 2027 rules](https://conf.researchr.org/track/fse-2027/fse-2027-papers) require 18 content/figure pages plus four references, single-column ACM formatting, anonymity, and Data Availability after Conclusion outside the content limit. The latest inspected PDF has 21 pages: Conclusion ends on 18; references occupy 19–21. The earlier 22-page state was superseded during review. Class options are correct and PDF metadata contains no author identity. Research AI use is described in Methods, including assistant-written drivers, analysis and perturbations; preserve it. The official AI section exempts writing assistance from disclosure and prohibits reviewer-manipulation instructions.

Before submission, rebuild the final source; recheck page landmarks and figure wording; verify every bibliographic record; inspect all public package bytes/metadata for anonymity and agreement with Data Availability. The anonymous URL could not be opened by this browsing tool, so live accessibility/anonymity is **unverified**, not failed. Stop submission if the final PDF exceeds the limit, the artifact reveals identity, or a claimed retained outcome lacks normal paired execution. Otherwise complete the listed revisions; no new experiment is required merely to maintain the present bounded claims.

## Inspected version fingerprints

SHA-256 values below bind this review; later changes need a delta check. The source-set hash is SHA-256 of the exact output of `sha256sum main.tex sections/*.tex` from the manuscript directory.

```text
source-set 36cec6fde00dfc05ca1a00bdee641fd7366520bdb12dee1cec2c16b1417012f4
main.tex 2e2d8fe42dcf47ae5f3c3bed6ced5a09da0569edb0e2ec507657e0bba41dc5e2
main.pdf cca2f20a5ec9d76c3d08027043b1cb929b27b4362cb5292c19c58c9649729cd1
FINAL39_SUMMARY.json c4c1ff2d5612833db10079fb8411bcb9159166abe4b73b62cd683057ce7bdf12
AUXILIARY_SUMMARY.json 5f10730e14dce4659fca939c2b11361873c0c331fc371475c8ec0d1ed78d7255
SELECTION.json d104fe75e9860800f3c269be0447e5acadc94d07a1dbc9a0fdcb26651e13cc35
INITIAL_EVIDENCE_CATALOG.json c80aaa851a33111ae0617e2bf154b34e97fe3ace95583bbe7544bf76cc98a54d
NATIVE_EVIDENCE_AVAILABILITY_STATUS.json 3967f1ad82c5d9f78c22e4a4056baf6ef9e118a8ce646fe6aa04ab66e54c8260
motivating-current-main/RESULT.json 4ee2a258b782e11bef5ed4a6c83f6195635da4322887b0a1685a9261b147cd09
motivating-retained-automatic/RESULT.json ababdc064260546594d84788bde923803910a4d43b12007fcd299ca0ab7a65f2
```
