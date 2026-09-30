# Author-redrawn SemWeaver figures

These are the author's three completed drawings from the first three slides of
the SemWeaver section (pages 6-8 of the full source deck).

## Current sources

- `semweaver-figures.pptx`: editable subset containing only these three slides.
  Unrelated sections, notes and author document properties are excluded.
- `source-slides.pdf`: the author's Mac PowerPoint PDF export, limited to the
  same three slides. This is the authoritative rendering; do not replace it
  with a LibreOffice export that substitutes fonts.
- `motivating-example.pdf`, `architecture.pdf`, `study-design.pdf`: vector
  crops of that PDF. Only outer whitespace is removed, with 4 bp safety padding.
- Corresponding PNGs are previews. The PDFs, not screenshots, are used for print.
- `IMPORT_MANIFEST.json` and `EXPORT_MANIFEST.json` bind inputs, slide selection
  and crops. No diagram content, result or model output was synthesized here.

All three drawings are used in the manuscript: motivating example, architecture,
and the study design in Subjects. The former operational-definition table is
merged into the Metrics prose; experimental values and definitions are preserved.

## Reproduce the crops

Install `pypdf==6.1.1`, then run `python3 export_from_pdf.py` in this directory.
This reads the retained three-page PDF and changes page boxes only. It requires
no font downloads, PowerPoint installation, API key or model call.

The PPTX uses Comic Sans MS, Consolas and Nanum Brush Script. They are not bundled
as standalone fonts. The author's PDF already contains the needed font subsets;
no server-side font substitution is needed to use it in the paper.

## Study-design caption qualifications

The 39-case comparison uses the same starting checker, patch and object scope.
The noisy-15 view is a view of E1, not another experiment. KNighter's actual
no-report branch performs no edit on 24 initially silent cases. The shared
32-reply ceiling has recorded natural-stop reuse qualifications; actual calls
and tokens are not matched. E3 uses the original 12 cases, two arms and three
decodes: 24 cells reused from E1 and 48 additional cells. E4 uses those same 12
inputs, three native-only model configurations, one decode each and a 2-reply
cap. CodeQL is separate and is not compared with KNighter. The detailed
starting-status/hash selection rule and G08 correction are in Subjects.

The author supplied a corrected PDF on 2026-09-30 with `extent N` in Figure 1.
The author then supplied a revised Figure 2 PDF on the same date. The retained
three-page PDF combines the corrected first slide, revised second slide and
unchanged third slide; only the second slide required a new crop box. The editable
PPTX is the earlier author version and may still contain `extend N`; the PDFs are
authoritative. Figure 3 remains byte-identical.
No diagram text was edited by the cropping process.
