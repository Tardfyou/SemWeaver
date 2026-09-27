# Final motivating figure: readable same-layout version

This is the larger-type version for the single-column FSE paper. It retains the original layout: A on the left, B above C on the right, six black-number/yellow callouts, purple headings, orange unsafe flow, teal safe flow, solid control-flow arrows and dashed semantic links. The earlier smaller-type rendering remains outside the final package as a local reference.

Use `fig-motivating-g21.svg` for editable shapes/text, `fig-motivating-g21.pdf` for LaTeX, and the PNG as a redraw reference. `PANEL_TEXT.json` contains copyable panel text. The exact generator is `../../render_motivating_example.py`.

## Geometry

The logical canvas is 1160 by 620, with origin at the upper left.

| Panel | x | y | Width | Height |
| --- | ---: | ---: | ---: | ---: |
| A | 10 | 58 | 612 | 500 |
| B | 639 | 58 | 511 | 215 |
| C | 639 | 287 | 511 | 271 |

Code uses DejaVu Sans Mono, 20 logical-point units. A has 22-unit line spacing; B/C have 20. Line numbers are 14 units. Headings are DejaVu Sans bold, 20 units; callout text is 16 and numbered discs use 15. Scale the complete graphic proportionally. At approximately 5.4 inches wide in the paper, code is roughly 6.7 pt.

Colors: purple `#79238f`, orange `#e59b00`, teal `#008e64`, neutral gray `#92989e`, yellow `#ffcf27`, pale orange `#fff1d2`, pale teal `#e3f5ed`. Panel borders are dashed black. The SVG preserves editable text, not outlines.

## Callouts and meaning

1. **capacity N**, M2: the actual array member's extent.
2. **bound B = N**, M5: the original loop can reach the unsafe last look-ahead.
3. **look-ahead i+1**, M11: the access shared by both revisions.
4. **bound B = N-1**, M6: the patched bound excludes that last iteration.
5. **missing bound proof**, B4–B7: the baseline overlooks loop-bound/capacity coupling.
6. **capacity-aware predicate**, C3–C6: establish the supported index/extent relation before suppressing the warning.

M2 is the member declaration from `struct dc`; the surrounding structure is omitted. `N` is a presentation alias for `MAX_PIPES * 2`, not an added Linux variable. Whitespace is compressed for readability. A is abridged patch/source code, and B/C are abridged checker logic, not a verbatim detector diff.

The original Linux pair is **1/1 before and 1/0 after**. All checker edits are model-produced; the final illustration includes development-guided quality continuations, totalling 14 model calls. Its 114 diagnostic executions include 60 parameterized variants of 20 forms, not 114 independent bugs. Failed intermediate candidates are retained. No claim of universal non-overfitting, arbitrary-loop coverage or cross-project recall is made. This illustration does not replace any frozen main-study result.
