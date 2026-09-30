# Figure redraw guide

**Current final Figure 2 and study-design drawing:** use
[`redrawn/README.md`](redrawn/README.md), its PDF/SVG/PNG files and the native
editable PowerPoint. These two figures were independently redesigned without
the previous style template. The older architecture instructions below are
historical; Figure 1 and the E3 data-matrix instructions remain applicable.

Use the current vector PDFs as geometry references. PNG previews are 4x renders,
not replacements for vector output. All dimensions below are PDF points (bp).

| Figure | Vector / editable source | Canvas | Paper page |
|---|---|---|---:|
| 1: motivating example | `fig-motivating-g21-f258.pdf` / `.tex` | 378 x 218 | 5 |
| 2: architecture | `fig-architecture-f258.pdf` / `.tex` | 378 x 225 | 6 |
| 3: complete E3 matrix | `fig-e3-repeats.pdf` / `.tex` | 378 x 174 | 15 |

## Non-data diagrams

Figure 1 keeps one left panel and two stacked right panels. Keep the six numbered
callouts and the distinction between solid bound-to-access links and dashed semantic
links. The abbreviations `N` and `hr_match` are explained in the caption, not new
program identifiers. The lower-right panel includes callout 6 within its gray
background. Preserve the 1/1 versus 1/0 labels and the separate 14-response example
boundary; this is not the one-response main-study checker.

Figure 1 now implements the agreed colors: 2-to-3 is orange `#D97732`, 4-to-3
is green `#388443`, and all three cross-panel dashed arrows are gray `#666666`.
These are bound/access relations and correspondences, not a complete control-flow graph.

Figure 2 is redrawn as three horizontal stages, not the old three tall columns:

1. Four equal frozen-input boxes feed a shared collection bus.
2. Three equal provenance boxes feed one full-width typed evidence bundle.
3. One left-to-right chain edits the latest candidate, performs the paired scan,
   and updates the separately retained checker. One dashed orange feedback lane
   returns latest code and validation feedback below this row; it does not pass
   through a box or the retained output.

Do not merge the latest candidate with the retained checker. Preserve unavailable
evidence, output/source-derived provenance, and the native-only fixture boundary.
The bounded-view arrow is black because the bundle is mixed provenance, not all native.

| Role | Stroke / label | Fill |
|---|---|---|
| Native evidence | `#356E9A` | `#E9F1F7` |
| Source context / neutral inputs | `#666666` | gray 0.949 |
| Diagnostics, validation and feedback | `#D97732` | `#FFF1E5` |
| Retained checker | `#388443` | `#E9F4EB` |
| Editor and mixed bundle | black | white |

Color indicates role, not correctness or measured performance. The labels and
solid/dashed line distinction remain interpretable without color.

For manual reproduction, use the 378 x 225 bp canvas with a top-left origin:

| Element | Left x positions | Top y | Width x height |
|---|---|---:|---|
| Four inputs | 0, 96, 192, 288 | 18 | 90 x 21 |
| Three evidence boxes | 0, 129, 258 | 65 | 120 x 35 |
| Mixed evidence bundle | 0 | 112 | 378 x 20 |
| Editor / validation / retained | 0, 132, 272 | 157 | 106 / 114 / 106 x 30 |

Section-title centers are at y=8, 52, 142; the feedback lane is y=200. Footer
baselines are y=213 and 223. Main labels are 8.039 bp and footer/feedback labels
7.3 bp. Borders use 0.65/0.8 bp; main arrows use 0.8626 bp. Exact arrow bends
are in the TikZ source. Keep text within boxes rather than stretching the canvas.

Diagram prose uses Nimbus Roman at 8.039 bp; code uses Courier at 7.1 bp.
Use the exact colors, stroke widths and arrow geometry in
`f258-diagram-style.tex`. These are open-font adaptations, not pixel-identical
copies of the style reference. The new Fig.2 topology and semantic colors are a
readability adaptation, not a source-strict replica. Re-render after manual changes and check labels,
callout circles, arrow routing, bottom legends and the actual 378 bp inclusion width.

## Data figure and tables

Figure 3 contains all 36 paired outcomes, including ties and disadvantages. A cell
is `native vulnerable/fixed : no-internal vulnerable/fixed`. Signs/colors are
comparative directions, not significance. `fig-e3-repeats.json` contains all cells.
Regenerate the editable diagram from the recorded summary with:

```sh
python3 figures/build_e3_repeat_figure.py --summary ../study-view/AUXILIARY_SUMMARY.json --output figures/fig-e3-repeats.tex
latexmk -pdf -jobname=figures/fig-e3-repeats figures/e3-repeats-standalone.tex
```

The JSON provenance hash reflects the exact input summary bytes; a text-masked
summary can change that hash without changing the validated numerical matrix.
The generator checks all cells and the full outcome totals before rendering.

Tables remain editable LaTeX in `sections/03-approach.tex`,
`sections/04-evaluation.tex` and `sections/05-results.tex`. Table 4 combines the
full 39-subject comparison and the complete noisy-15 stratum. Table 5 reports all
three CodeQL decodes and their model costs; Table 6 includes the initial state.
Keep numeric precision and all denominators. Do not stretch screenshots into tables.

## Build

From the paper directory, use pdfLaTeX (via `latexmk -pdf`). The standalone
wrappers are `motivating-f258-standalone.tex`, `architecture-f258-standalone.tex`
and `e3-repeats-standalone.tex` under `figures/`. Build the main paper afterward.
Do not change the ACM body font, margins or line spacing to accommodate a redraw.
