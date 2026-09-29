# Figure redraw guide

Use the current vector PDFs as geometry references. PNG previews are 4x renders,
not replacements for vector output. All dimensions below are PDF points (bp).

| Figure | Vector / editable source | Canvas | Paper page |
|---|---|---|---:|
| 1: motivating example | `fig-motivating-g21-f258.pdf` / `.tex` | 378 x 218 | 5 |
| 2: architecture | `fig-architecture-f258.pdf` / `.tex` | 378 x 225 | 6 |
| 3: complete E3 matrix | `fig-e3-repeats.pdf` / `.tex` | 378 x 174 | 15 |

## Non-data diagrams

Figure 1 keeps one left panel and two stacked right panels. Keep the six numbered
callouts and the distinction between solid control-flow links and dashed semantic
links. The abbreviations `N` and `hr_match` are explained in the caption, not new
program identifiers. The lower-right panel includes callout 6 within its gray
background. Preserve the 1/1 versus 1/0 labels and the separate 14-response example
boundary; this is not the one-response main-study checker.

Figure 2 has three panels: frozen inputs; provenance views; edit and retain.
Do not merge the latest candidate with the retained checker. Preserve unavailable
evidence, output/source-derived provenance, and the native-only fixture boundary.
The feedback arrow runs outside the right-hand boxes.

Diagram prose uses Nimbus Roman at 8.039 bp; code uses Courier at 7.1 bp.
Use the exact colors, stroke widths and arrow geometry in
`f258-diagram-style.tex`. These are open-font adaptations, not pixel-identical
copies of the style reference. Re-render after manual changes and check labels,
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
