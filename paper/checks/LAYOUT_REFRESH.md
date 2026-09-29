# Layout refresh and editorial change note

This pass changes presentation, not experimental outputs or study membership.
The full recorded-study verifier passed after workspace cache cleanup: 357 paired
record occurrences and 162 refinement cells; these are audit counts, not new
experimental denominators. No new model or analyzer execution was requested.

## Current display map

| Display | Placement | Content |
|---|---:|---|
| Figure 1 | 5 | Automatic motivating example; corrected abridged branch and callout background |
| Figure 2 | 6 | Architecture, after the Approach heading |
| Table 1 | 7 | Rule-guided evidence requirements |
| Algorithm 1 | 8 | Same refinement operations, compact inline conditional statements |
| Table 2 | 8 | Evidence origin definitions and counts 81/51/61 |
| Table 3 | 12 | Operational warning-outcome definitions |
| Table 4 | 13 | Full 39 comparison and complete noisy-15 stratum in two panels |
| Table 5 | 14 | All three CodeQL decodes, replies, tokens and recorded agent time |
| Figure 3 | 15 | All 12 subjects x 3 paired decodes, not selected successes |
| Table 6 | 15 | Initial state and three limited-budget model configurations |

The final compiled PDF has 21 pages: body, AI statement and Data Availability
end on page 18; references occupy pages 19-21. ACM body typography and margins
are unchanged. Section-boundary float barriers keep diagrams/results in their
sections; subsection barriers that produced empty float pages were removed.

## Checks and remaining distinctions

- pdfLaTeX compilation succeeded, with no overfull boxes, undefined references
  or duplicate labels. Bibliography missing-field warnings remain; they are not
  figure clipping or failed compilation.
- The small paper archive was extracted into a new directory and built from
  scratch with networking disabled. All 21 pages' extracted text matches the
  reviewed PDF; the build does not depend on leftover local bibliography/cache files.
- All six tables, the algorithm and three figures were inspected at final size.
  Diagram glyphs are inside their standalone PDF bounds; arrows/callouts were
  checked visually. PDF glyph containment is not a proof of all semantic relations.
- Main, noisy-subset, evidence-origin and model values remain unchanged. The
  E3 matrix is generated from all 36 recorded pairs with category-total assertions.
  Initial model-subset values and CodeQL cost values were promoted from existing
  recorded summaries/prose into tables, not newly measured.
- The original paper-writing AI sentence is restored verbatim after Conclusion
  and before Data Availability. Its heading is neutral, not an acknowledgment.
  The separate research-AI disclosure remains in Experimental Environment.
- The reference's line/color/typographic conventions remain local to displays;
  open-font and content-geometry adaptations remain disclosed in the style report.

## Meaning-preserving editorial changes

Reading coverage: edited Approach, motivating-example caption, Evaluation,
Results, Discussion, Conclusion and end matter, with their equations, display
sources and the recorded main/auxiliary results. No experiments were added.
At the user's follow-up request, two relevant citations were added to Related
Work after primary-source verification, as recorded below. The existing
full-paper prose review remains a separate earlier pass.

- Results: repeated values moved into adjacent tables; the entire E3 outcome
  distribution is expanded into a matrix. Same denominators, adverse outcomes,
  selected/reused-subject boundaries and attribution limits remain explicit.
- Approach: repeated type-name enumeration is merged with type definitions;
  origin counts are cross-referenced to Table 2. No origin is reclassified.
- Discussion: the repeated scope paragraph is merged into the precision/scaling
  paragraph. Changed-fork tests, target correctness, noise cost, limited model
  repetitions and unmeasured labor remain adjacent to the supported claims.
- Conclusion (S1/S5): grammatical focus becomes the auditable checker-editor
  contribution, retaining every comparison and the limits on precision, target
  recall and generalization. This is a small scope-preserving editorial choice,
  not evidence of higher review scores.
- S6 equivalence check: compression was checked against unchanged tables and
  method conditions. No significance, causality, universal superiority or
  reviewer-score claim was introduced. The requested AI sentence was not edited.

## Requested citation additions

- `li2024llift`: Li, Hao, Zhai and Qian, *Enhancing Static Analysis for Practical
  Bug Detection: An LLM-Integrated Approach*, PACMPL 8(OOPSLA1), Article 111,
  26 pages (2024), DOI 10.1145/3649828. Bibliographic identity and the
  post-constraint-guided UBI mechanism were checked against the
  [authors' published paper](https://www.cs.ucr.edu/~zhiyunq/pub/oopsla24_llift.pdf).
  The ACM landing page returned 403; this was not interpreted as missing literature.
  The earlier differently titled arXiv version was not cited as a separate study.
- `guo2025repoaudit`: Guo, Wang, Xu, Su and Zhang, *RepoAudit: An Autonomous
  LLM-Agent for Repository-Level Code Auditing*, ICML 2025, PMLR 267:21083-21100.
  Authors, venue, pages and repository-auditing scope were checked against the
  [official proceedings](https://proceedings.mlr.press/v267/guo25n.html).

Both citations support specific contextual sentences, not our numerical results.
No cross-paper performance comparison or new first/universal claim was introduced.
This is a targeted citation update, not an exhaustive novelty search or new
baseline evaluation. The bibliography now contains 53 cited entries (previously 51).
