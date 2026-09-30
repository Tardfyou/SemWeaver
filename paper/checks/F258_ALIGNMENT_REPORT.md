# F258 component-level alignment and residual differences

Scope update: the final Figure 2 and study-design drawing now use an independent
design and exporter under `figures/redrawn/`, at the user's explicit request.
They are not F258 replicas or adaptations. The architecture descriptions below
record superseded iterations; table styles, Figure 1 and the E3 matrix retain
their existing design. See `figures/redrawn/README.md` for the final pair.

Reference PDF SHA256:
`4087ef12f47e73dbe2a4c412787ab3a15b6ffaa721cc587f0d114964edec4711`.
The original physical pages5/11/12 were rendered and measured in
`visual-audit/reference-f258`. Source article text/fonts are not redistributed.
The original style pass inspected seven tables and two generated figures. The
current refresh has six numbered tables (seven tabular panels) and three figures;
its placements and additional checks are recorded in `LAYOUT_REFRESH.md`.
All current displays were inspected;
no unused statistical plot or synthetic experimental value was added.

## Tables: physical-page11 Table II reference

| Property | Source | Current output / disposition |
|---|---|---|
| Typeface/size | NimbusRomNo9L-Regu,8.369bp | Same text face and8.369bp in all table panels; code tokens and mathematical glyphs retain their appropriate fonts. |
| Rule width | .33432bp | .33430bp measured; .00002bp residual from the TeX/PDF rule serialization. |
| Double-rule center spacing | 2.00844bp | 2.00840bp measured; .00004bp residual. |
| Body content height |10.54bp|10.54bp base strut; row separators add .33430bp. Wrapped prose rows are taller rather than clipped/shrunk. |
| Topology | Double top/bottom; group dividers; no outer box | Preserved in all table panels; two-level merged main-result header has local clines that avoid the multirow labels. |
| Colors | Four five-decimal RGB tokens | All four source tokens are defined. Actual F1 cells occupy two bins and emit exact(.79138,.88196,.72548) and(.95609,.72275,.72275), without forced extreme/synthetic values. |
| Threshold mapping | Original mapping code unknown | Explicit adaptation shades1-F1 with fixed.25/.50/.75 thresholds. Counts, unknowns and unvalidated target semantics are not heatmapped. Colors do not encode significance. |
| Width/captions |496.56bp cross-column source table; NDSS numbering|Widths adapt to actual columns/content and FSE single-column space. ACM caption formatting and Arabic numbering remain mandatory venue defaults. |

The initial XeTeX PDF rounded RGB to three decimals even with increased PDF
coordinate precision. The final pdfLaTeX build preserves the five-decimal
source RGB; it also meets the page limit. No body margin or global font-size
override was used to achieve this.

## Figures: physical-page5 Figure2 reference

The original diagram style used measured .949/.851 gray regions, rectangular
structure, black .8626bp strokes and source orange/green fills. The requested
Fig.2 redraw now uses an explicitly **adapted-readable** layout: three horizontal
stages, aligned rectangular boxes, role-specific dark strokes and pale fills.
Fig.1 keeps its original code fills but implements orange/green bound-to-access
arrows and gray dashed correspondences. These new semantic colors are not
claimed to be source-paper tokens. See `figures/REDRAW_GUIDE.md` for exact values.

- Architecture: four frozen inputs, three provenance classes, one mixed bundle,
  then the edit/scan/retain chain with a separate labeled feedback lane. The latest
  candidate and retained checkpoint stay separate. Optional observations, the
  native-only guard and target-correctness boundary remain explicit. Panel a/b/c
  references are retained; no new algorithm or experiment is implied.
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
Roman8.039bp and Courier7.1bp are used for prose and code. The new architecture
footer/feedback labels are 7.3bp; border widths are 0.65/0.8bp. Math fonts differ.
These font/layout adaptations are explicitly not pixel-identical source copying.
G21's asymmetric topology is preserved because the user requested it, rather
than changing it into the source's three horizontal panels.

## Integrity and scope

Original manual architecture TeX/PDF remain byte-identical to their original
commit. Proposed architecture and G21 diagrams have separate filenames; old
automated diagrams remain available. Algorithm/equation formatting stays ACM
native because the reference has no corresponding evaluated algorithm component.

Main39, matched15 and model-subset outcomes, the five E3 category totals,
record origins81/51/61 and all denominators remain unchanged. The schematic
figures are not executions. The new E3 figure visualizes all recorded pairs;
it uses the existing pale green/red colors for comparative direction, not the
F1 quartile mapping. Data/trace hashes are not altered. The refresh's compile,
page, caption and local visual checks are recorded in `LAYOUT_REFRESH.md`.
