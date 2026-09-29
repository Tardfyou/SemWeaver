# F258 component-level alignment and residual differences

Reference PDF SHA256:
`4087ef12f47e73dbe2a4c412787ab3a15b6ffaa721cc587f0d114964edec4711`.
The original physical pages5/11/12 were rendered and measured in
`visual-audit/reference-f258`. Source article text/fonts are not redistributed.
All seven current tables and both automatically generated figures were inspected;
no unused statistical plot or synthetic experimental value was added.

## Tables: physical-page11 Table II reference

| Property | Source | Current output / disposition |
|---|---|---|
| Typeface/size | NimbusRomNo9L-Regu,8.369bp | Same text face and8.369bp in all seven tables; code tokens and mathematical glyphs retain their appropriate fonts. |
| Rule width | .33432bp | .33430bp measured; .00002bp residual from the TeX/PDF rule serialization. |
| Double-rule center spacing | 2.00844bp | 2.00840bp measured; .00004bp residual. |
| Body content height |10.54bp|10.54bp base strut; row separators add .33430bp. Wrapped prose rows are taller rather than clipped/shrunk. |
| Topology | Double top/bottom; group dividers; no outer box | Preserved in all seven tables; two-level merged main-result header has local clines that avoid the multirow labels. |
| Colors | Four five-decimal RGB tokens | All four source tokens are defined. Actual F1 cells occupy two bins and emit exact(.79138,.88196,.72548) and(.95609,.72275,.72275), without forced extreme/synthetic values. |
| Threshold mapping | Original mapping code unknown | Explicit adaptation shades1-F1 with fixed.25/.50/.75 thresholds. Counts, unknowns and unvalidated target semantics are not heatmapped. Colors do not encode significance. |
| Width/captions |496.56bp cross-column source table; NDSS numbering|Widths adapt to actual columns/content and FSE single-column space. ACM caption formatting and Arabic numbering remain mandatory venue defaults. |

The initial XeTeX PDF rounded RGB to three decimals even with increased PDF
coordinate precision. The final pdfLaTeX build preserves the five-decimal
source RGB; it also meets the page limit. No body margin or global font-size
override was used to achieve this.

## Figures: physical-page5 Figure2 reference

`f258-diagram-style.tex` uses measured .949/.851 gray regions, rectangular
structure, black .8626bp strokes, compact labels and the source orange/green
RGB triples(.957,.694,.514)/(.663,.820,.557). There are no shadow cards or
decorative source-paper icons. Solid versus dashed links retain explicit meanings.

- Architecture: three provenance/workflow panels, latest candidate versus
  retained checkpoint, normal paired execution, and an explicit native-only
  guard/target-oracle boundary. Body cross-references were updated to those
  panels, not left pointing to old d/e labels or a nonexistent request arrow.
- G21: the user's requested original left/top-right/bottom-right topology is
  retained. Array extent, vulnerable/fixed bounds and capacity test remain
  linked to the six callouts. Routing was adjusted after rendering so links do
  not strike through the callout text. The separate14-response budget,
  fixture feedback and main-checker distinction remain in the caption/body.

Standalone boxes measure377.9956×224.9974bp (architecture) and
377.9956×217.9975bp (G21), versus nominal378×225/218bp. This tiny difference
is coordinate quantization, not an asserted zero pixel error. Every extracted
glyph is inside the respective PDF page. The final paper's inclusion width
matches these nominal physical dimensions within that quantization.

The source Figure2 uses Times New Roman/Cambria glyphs; available open Nimbus
Roman8.039bp and Courier7.1bp are used for prose and code. Math fonts differ.
These font/layout adaptations are explicitly not pixel-identical source copying.
G21's asymmetric topology is preserved because the user requested it, rather
than changing it into the source's three horizontal panels.

## Integrity and scope

Original manual architecture TeX/PDF remain byte-identical to their original
commit. Proposed architecture and G21 diagrams have separate filenames; old
automated diagrams remain available. Algorithm/equation formatting stays ACM
native because the reference has no corresponding evaluated algorithm component.

Main39, matched15 and model-subset numerical rows, the five E3 outcome rows,
record origins81/51/61 and all labels/denominators remain unchanged. Both figures
are schematic views, not new executions. Data/trace hashes are not altered.
The final small prose pass is separate and must be followed by another compile,
page, caption, anonymity and local visual regression check before publication.
