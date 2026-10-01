# Current layout and author-figure handoff

All three author-redrawn figures are integrated from the Mac-exported PDF.
Only external white margins are cropped. No text, shape, color, font or result
inside the drawings has been rewritten. The original vector content streams are
unchanged, and the selected three-page source renders identically to the same
pages of the full author PDF. Other project slides are not included in the artifact.

## Current display map

| Display | PDF page | Content |
|---|---:|---|
| Figure 1 | 3 | Author motivating example |
| Figure 2 | 5 | Author architecture |
| Algorithm 1 | 6 | Paired-warning refinement |
| Table 1 | 6 | Rule-guided evidence requirements |
| Table 2 | 7 | Evidence origins: 81 / 51 / 61 |
| Figure 3 | 10 | Author study design and E1/E3/E4 reuse |
| Table 3 | 13 | Full 39 and complete noisy-15 comparison |
| Table 4 | 13 | All three CodeQL decodes and recorded costs |
| Figure 4 | 14 | All 36 paired E3 decodes |
| Table 5 | 15 | Initial state and three model configurations |
| Table 6 | 16 | Phase-separated terminal checkpoint outcomes |

The complete PDF has 22 pages. Main content, AI-use disclosures and Data
Availability end on page 18; References occupy pages 19-22. ACM body font,
margins and line spacing are unchanged.

## Table clearance correction (October 1, 2026)

Table 6 now uses three actual header rows instead of bottom-aligned short stacks;
the grouped rule no longer crosses the upper line of the column labels. A local
1 bp row-height allowance separates text from rules without reducing the font.
Table 5 adds header height for the mathematical F1 label. Existing F258-style
rules, colors and numeric cells are unchanged. Tables 1--6 and the E3 matrix were
checked at final PDF size; no comparable text/rule collision was found in the
other data components. The author-owned Figures 1--3 were not edited.

## Edits supporting the added study figure

The former operational-definition table is merged into Metrics prose. Warning
recovery, fixed-report reduction, PDS, rejected signal loss and comparative
disadvantage remain defined; equations and acceptance conditions remain intact.
Repeated protocol summaries are tightened, not removed from the actual Protocol
section. Model/programmatic boundaries and prompt inputs remain enumerated.
Repeated Discussion counts now refer to RQ1; all original numerical results,
noise costs and limitations remain in the Results, tables and conclusion.
Related Work retains the same citation coverage and substantive distinctions.

## The 12-subject selection

Subjects explicitly records starting-status stratification and deterministic
ranking, not refinement-outcome selection: sort SHA-256 of
salt + ':' + case_id in the original 14 noisy / 25 silent strata and take 4 / 8.
The fixed salt is semweaver-fse2027-e3-e4-repeat-v1.
The source screen hash is
d89fc9d6c6140f0329d118fdbf4f02ac99b96cc50e0a917699c15edfa980b798.
Repairing G08's scan changes its start from 0/0 to 2/2 and the retained subset
to 5 noisy / 7 silent, without replacing a subject. It is not a
vulnerability-class-stratified or independent confirmation set.

## Verification and cleanup

- The paper compiles with no overfull boxes, undefined references or duplicate
  labels. Bibliography missing-field warnings remain distinct from compilation
  or figure errors.
- The isolated paper archive is rebuilt offline; all page text is compared with
  the reviewed PDF. The build uses embedded PDF fonts, not server-installed
  Comic Sans/Consolas substitutions.
- The old generated non-data figures, builders, wrappers and preview trees were
  removed from the active paper and publication directories. The current E3 data
  matrix, its generator, all experimental evidence, original manuscript backups,
  Git history and sealed snapshots are retained.
- The current figure sources and crop-only reproduction instructions are in
  figures/manual/README.md, IMPORT_MANIFEST.json and EXPORT_MANIFEST.json.
- The author's full original PPTX and PDF were preserved unchanged outside the
  public package. Only the selected SemWeaver slides are public.
- The original Acknowledgments heading and AI-use sentence remain verbatim.
  The separate research-AI disclosure remains in Experimental Environment.
- No experiment was rerun or relabelled for this layout change. Original adverse
  outcomes, reuse, sample denominators, model budgets and interpretation limits
  remain in the paper.
