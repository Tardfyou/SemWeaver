# Format, data-display and citation audit — 2026-09-30

Scope: the current submission manuscript, five tables, one numerical matrix,
all 59 displayed references, and the retained evidence underlying reported
outcomes. No model calls, new experiments or scored-checker changes were made.
This is a bounded integrity audit, not a guarantee of acceptance or a claim
that every assertion in every cited paper has been independently proved.

## Venue format

Checked against the live [FSE 2027 Research Papers CFP](https://conf.researchr.org/track/fse-2027/fse-2027-papers).

- `acmart` uses `acmsmall,screen,review,anonymous`, single column. Body font,
  margins and caption conventions are retained; F258 affects local displays.
- The reviewed PDF has 22 physical pages. Conclusion, the original AI-writing
  statement and Data Availability fit on page 18. References occupy pages
  19–22, within the four-page allowance. Data Availability is after Conclusion
  and is independently excluded from the limit by the CFP.
- Numeric citations are permitted. `acmnumeric` and `ACM-Reference-Format`
  determine alphabetical bibliography numbering, not first-citation order.
- No unresolved references, missing-character or overfull-box warnings occur
  in the final main compilation. BibTeX still reports optional missing
  publisher/address fields and some unpaginated publication fields. Those
  warnings are not fabricated references and were not hidden with invented
  metadata.
- Author names/affiliations are anonymous; the artifact URL is the anonymous
  mirror. Original three author-exported figure files remain unchanged.
- The CFP encourages omitting acknowledgments, but does not prohibit them.
  The author expressly requested the original `Acknowledgments` heading and
  AI-writing sentence verbatim; both remain. That section has no identifying
  thanks or funding information. Methods separately describes research AI use.

## Data displays: reference versus output

Reference: F258 Table II, physical page 11; reference PDF SHA-256
`4087ef12f47e73dbe2a4c412787ab3a15b6ffaa721cc587f0d114964edec4711`.
The reference PDF and final output were rendered and their PDF geometry read.

| Component | Reference / target | Final output / handling |
|---|---|---|
| Tables 1–5 text | Nimbus Roman, 8.369 bp | 8.369 bp; existing code identifiers remain monospaced |
| Table rules | 0.33432 bp | source 0.33432 bp; PDF serialization 0.33430 bp |
| Double rules | 2.00844 bp center spacing | local edge gap 1.67412 bp plus rule width; rounding only |
| Table row baseline | 10.54 bp base | retained; wrapped text rows expand naturally |
| F1 fills | original four five-decimal RGB tokens | tokens retained exactly; only the two bins actually reached appear |
| Tables 1–2 | grouped text columns | ragged-right wrapping removes stretched word gaps; all labels/counts retained |
| Table 3 | grouped header and two panels | unchanged complete-39 and noisy-15 data, no outer box; natural panel widths retained |
| Table 4 | grouped vertical separators | outcome and cost groups separated; redundant cost-column rules removed |
| Figure 4 matrix | data-table font and direction colors | 8.369 bp at final size; 378 bp natural width, no implicit enlargement |
| Figure 4 values | exact 36 paired outputs | fixed anchors align sign, native pair and control pair; no values changed |

Figure 4 is an explicit adapted matrix, not a copy of the original paper's
matrix content or topology: gray means tie/other, green native comparative
benefit and red comparative disadvantage. The legend names native/control
recovery and report-reduction counts without suggesting loss of an initial
hit or statistical significance. Counts are generated from the outcome data.
Tables shade the documented quartiles of `1-F1`, not an inferred reference-paper
threshold algorithm. ACM Arabic captions and single-column placement take
precedence over the reference venue's layout. No claim of pixel identity is made.

Final locations: Table 1 page 7, Table 2 page 8, Table 3 page 13, Table 4 page 14,
Figure 4 and Table 5 page 15. All six table panels and the complete matrix were
visually inspected, together with the final content/reference boundary.
The Figure 4 data JSON is byte-unchanged, SHA-256
`44ae5a3db1659ef93a64ae4c8238ac11b2533d4da5687b4b3804847e3bae5171`.

## Citations and corrections

All 59 displayed bibliography keys resolve with consecutive numbering and
matching auxiliary/bibliography order. All citation contexts were inspected.
Metadata was compared to retained publisher deposits for 35 entries and live
primary sources for the remaining 24, with additional live checks for ambiguous
records. No fabricated displayed work or wrong DOI/year/venue/page value was
identified. This does not mean all 59 complete papers were reread.

Corrections made:

1. Self-Refine and Reflexion now support the general feedback-refinement
   statement; compiler/execution feedback is specifically attributed to KNighter.
2. Industrial deployment is attributed to static-analysis frameworks, not to
   both named analysis paradigms without supporting citations.
3. External publications no longer appear to substantiate our own fixture
   result. Khoury is cited with generated-code security validation instead.
4. The learning-based model list no longer claims graph models without a
   directly corresponding citation in that list.
5. Livshits' published name is completed to V. Benjamin Livshits.
6. Publisher-provided DOIs are added for Self-Refine, Reflexion, Toolformer and
   Tree of Thoughts. Their titles, years and reported results are unchanged.

QL's publisher PDF and publisher BibTeX disagree on its last page. The existing
official BibTeX range is retained rather than silently guessing a correction.

## Numerical and implementation evidence

Independent reaggregation checked E1, the reused noisy-15 view, E4 metrics and
received replies; 237 summary-row objects include reused views and are not 237
independent experiments. All 36 E3 count tuples match 72 retained condition
cells. The original fixed-salt subset selection, corrected composition and
declared budgets reconstruct from the recorded inputs.

The audit also checked 193 initial provenance records, 150 dynamic attempts,
1,744 recorded file hashes, CodeQL outcomes/tokens/timing/RSS, and the motivating
example's 114-fixture result and 14-call lineage. The main checker's separate
74/114 fixture outcome is not replaced with the illustrative result.

One protocol sentence was corrected: a permitted reply is an edit *attempt*,
not necessarily an actual edit. The charged thinking-only reply was already
properly retained in the results and budgets. No numerical result changed.

Remaining evidence limits are explicit: version-level warnings are not verified
target recall; G24 target preservation is unverified; native causality and
unseen-family generalization are not established; model names do not verify
immutable backend weights. These limitations are not removed by formatting.

The author-owned Figure 1 still contains the callout `extend N` where `extent N`
is intended. It is a known spelling issue, not changed experimental data; the
protected native PDF export has deliberately not been edited.

## Release validation

The paper-only archive was extracted into a fresh directory and compiled with
network access disabled. All 22 rebuilt page texts equal the reviewed PDF;
the final build has no overfull, unresolved-reference or missing-character
warnings. All 144 immutable experiment transport files retain their prior
hashes. The three author figure sources and numerical matrix JSON are unchanged.
