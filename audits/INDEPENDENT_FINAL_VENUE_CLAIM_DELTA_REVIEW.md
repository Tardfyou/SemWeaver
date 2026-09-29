# Independent final venue and claim delta review

2026-09-29, approximately 19:07–19:15 CST. Verdict: **REVISE, minor wording only**. The required earlier fixes, numerical tables, figure references and observed PDF submission format pass this affected-role check. Two residual wording issues below should be corrected without changing results. This verdict contains no score or acceptance prediction.

I inspected the frozen original-based manuscript, all sections, bibliography, both included figure sources/PDFs, table/diagram styles and the current compiled PDF. I independently recalculated main portfolio metrics from `FINAL39_SUMMARY.json` and checked auxiliary outcomes against `AUXILIARY_SUMMARY.json`. I did not read author strategy/remediation conclusions or other reviewer reports. No model calls, experiments, publication or manuscript edits occurred.

## Earlier required fixes resolved

- Background line 23 now says generated checkers “can omit” conditions; line 27 describes inference from patch, source and outputs; line 39 explicitly defines operational checks rather than target-correctness proof.
- Figure 2 has “Edit and retain,” not target-preserving refinement. Its caption and footer distinguish warning retention from a target oracle. Body references now consistently use panel (b) for provenance/bundle and (c) for refinement; there are no obsolete (d)/(e) references.
- Figure 1 caption explicitly identifies the separately budgeted 14-response, fixture-informed illustration and excludes substitution for the one-response main checker. M5/M6, M10/M11, B1–B5, C3–C6 and numbered callouts 1–6 agree with the diagram. Its array alias, bounds, colors and solid/dashed conventions agree with the prose.
- Discussion opening and Conclusion qualify reduction to the 15-case stratum and disclose increased full-cohort burden.

## Two remaining wording edits

1. `sections/02-background.tex:24` still says the generated checker “often captures only the syntactic shape.” The reviewed evidence does not measure that prevalence. Replace “often captures” with “can capture.” This also avoids reintroducing the generality removed from line 23.
2. Table 3, `sections/04-evaluation.tex:50`, defines rejected signal loss as “A previously present vulnerable warning becomes absent.” This can imply preservation of each warning. Equation Accept requires nonzero vulnerable-side warning presence; Threats explicitly states that it does not preserve the entire set. Use “A previously warning-positive vulnerable revision becomes silent; rejected.” This is a precision fix to the operational definition, not a new metric.

The conditional “afternoon” adaptation estimate remains unmeasured and should not be treated as evidence; the manuscript labels it accordingly. It is not a newly demonstrated engineering result.

## Numbers, styling and scientific boundaries

All seven tables and both figures are present. Reaggregation confirms native/control/KNighter TP 23/21/15, FP 18/13/13, PDS 5/8/2, F1 .5750/.57534/.44776, fixed reports 123/68/69 and replies 150/133/179. The 15-case stratum retains TP15/PDS2/F1.6977 with fixed reports 44/68/69. E3 has 36 complete pairs on 12 subjects, category counts 4/1/1/2/28, five always-tied and seven mixed subjects. E4 values and received replies 24/23/21 match the auxiliary summary. Color uses disclosed fixed bins of 1−F1, not significance or target correctness; native/control's near tie receives the same shade.

The strongest scientific limits remain visible: native increases overall noise; G24 dominates reduction without verified target preservation; G08 demonstrates target mismatch; no E3 subject benefits in all three decodes; selection/reuse overlaps the evaluated cohort; KNighter does not edit the 24 no-report cases. Static records and per-attempt dynamics remain separate. The CodeQL 22/22 third decode, exact inherited filters, one-case costs, 12-subject model scope and fixture-informed 114/114 illustration remain bounded. The 74/114 main checker is not replaced. Styling and reordered abstract sentences introduce no stronger efficacy claim.

## Venue, references and publication boundary

The [official FSE 2027 rules](https://conf.researchr.org/track/fse-2027/fse-2027-papers) remain satisfied by the observed PDF: single-column acmsmall/review/screen/anonymous, 21 total pages, Conclusion on 18, Data Availability immediately afterwards, references on 19–21. Table styling is local; I found no body-margin/font override or overfull/undefined-reference warning. PDF metadata and visible manuscript contain no author identity. Research AI disclosure remains in Methods. If AI materially prepared research figures beyond presentation restyling, disclose that actual role there too.

All figure/table references resolve; 51 active citation keys exist in the bibliography and 51 entries render. Primary-source spot checks confirm VulGenie, IRIS, LLM4PFA and ZeroFalse metadata and the cited high-level descriptions. ACM DOI retrieval failed for KNighter and Chapman; those failures neither discredit nor authenticate their entries. This delta check is not a fresh authentication of all 51 references or a public-package anonymity audit. No publication or public synchronization was performed. Release should preserve these exact bounded claims and synchronize the rebuilt manuscript only after the two wording fixes and any resulting page/hash delta check.

## Frozen fingerprints

The source-set digest is SHA-256 of the ordered `sha256sum` output for `main.tex sections/*.tex references.bib packages.sty commands.sty figures/figstyle.tex figures/f258-table-style.tex figures/f258-diagram-style.tex figures/fig-architecture-f258.tex figures/fig-motivating-g21-f258.tex figures/fig-architecture-f258.pdf figures/fig-motivating-g21-f258.pdf`, executed from the manuscript directory. It was unchanged at the end of inspection.

```text
source-set a969a5dd1720a096106d55b9a890af7e424888a95d1d70d0a30f9967622501fd
main.tex 2a2e757a3ccc9e8ab42da9c076fa70002edd7a8ae84c0be7fe34ba8990fc0725
main.pdf 2d4ad9cc69556c932473a4db7bcb12da40f6b290aa864460864d00c06b56e8d7
references.bib ef14749404332b1852a191b3036e364fbbed7fa9f0bc2eca6f61f527738608a5
FINAL39_SUMMARY.json c4c1ff2d5612833db10079fb8411bcb9159166abe4b73b62cd683057ce7bdf12
AUXILIARY_SUMMARY.json 5f10730e14dce4659fca939c2b11361873c0c331fc371475c8ec0d1ed78d7255
```

## Final narrow disposition, 2026-09-29 19:20 CST

**PASS for this affected-role delta.** The two minor findings above are resolved: Background line 24 now uses “can capture,” and Table 3 defines signal loss as a previously warning-positive vulnerable revision becoming silent. Both changes are present in the source and compiled PDF and preserve the existing operational metric.

The independently inspected rebuild still has 21 pages: Figure 1 on 5, Figure 2 on 6, Tables 4/5 on 14, Tables 6/7 on 15, Conclusion and Data Availability on 18, references beginning on 19 and ending on 21. The 14-response caption, portfolio and auxiliary numbers are unchanged; PDF metadata contains no Author field. The compilation log contains no overfull, undefined, fatal or TeX-error line. No new claim, experiment or publication was introduced by this check. The scientific limitations and authentication/public-package boundaries stated above remain; this disposition is not an acceptance prediction or authorization to publish.

Current SHA-256 fingerprints use the same source-set procedure defined above:

```text
source-set 1552a254405cada85d6abeba249c90508a32b9c067bb57c83031df2f4eb089ec
main.pdf c285e637f98c5d681aafdd28f13559302ed796b4e8d03126e3f006fea5af6a9d
sections/02-background.tex d9aa70040bcb822ccec651ee57256e502233f021d975c526b7fb5159f36dd77b
sections/04-evaluation.tex 76f5ff1ca9a96e4b5bc562bc44f5d13820256a37a65ca6208b49d6a3a3989f73
```
