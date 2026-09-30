# Final editable figures

These two diagrams were independently redesigned from the method and study
protocol. They do not import a style skill, the previous TikZ diagram style,
or an image template. The experiment results are unchanged.

## Use these files

- `fig2-architecture.png` / `.pdf` / `.svg`: final Figure 2.
- `study-design.png` / `.pdf` / `.svg`: final study-design drawing.
- `editable-figures.pptx`: two slides, with editable rectangles, connectors and
  text boxes. No flattened picture is used as a substitute for editable objects.
- `final-figures.zip`: both drawings and the editable handoff in one download.
- `*.json`: exact positions, text and colors for every primitive.
- `build_figures.py`: shared-source PDF, SVG and PowerPoint exporter.

The PDF is the exact visual reference. SVG keeps text as text. PowerPoint uses
Arial; the PDF embeds the metrically compatible open Liberation Sans font.
Native PowerPoint objects are editable, but an application/font substitution can
change their appearance; compare edits with the supplied PDF before publication.

## Manual reproduction

Use rounded rectangles with a **3 pt corner radius**, no shadows, and simple
right-angle connectors. The architecture artboard is **378 x 224 pt**; the
study artboard is **378 x 288 pt**. Body labels are 8.3-9 pt, principal titles
9-10 pt, and the large cohort count 24 pt. Normal borders are 0.8 pt, arrows 1 pt.
PowerPoint objects are supplied at 2x these dimensions for comfortable editing.

| Role | Dark color | Light fill |
|---|---|---|
| Main text / forward flow | `#243448` | white |
| Secondary labels / neutral context | `#617084` | `#F3F5F8` |
| Native evidence / SemWeaver native | `#315F98` | `#EEF4FC` |
| Diagnostics and feedback | `#A66024` | `#FFF5E9` |
| Retained checker | `#267560` | `#EDF7F2` |
| Neutral outlines | `#B9C3CF` | white |

In Figure 2, the three top cards feed one evidence bundle. The main row reads
left to right; only the dashed amber feedback arrow returns to the editor. The
green downward arrow updates the separately retained checker. The native box
summarizes the measured CSA interfaces; traces are optional. The mixed bundle
is not colored as if every record were native. Missing evidence, the native-only
fixture guard and target-correctness limits remain defined by the method.

In the study drawing, all three E1 arms receive all 39 inputs. The original
12-subject subset is shared by E3 and E4, not two independent datasets. Gray E3
tile 1 reuses E1; blue tiles 2 and 3 are additional decodes, not better outcomes.
The separate CodeQL strip has no arrow implying inclusion in the CSA cohort.

## Captions and scope to preserve

**Figure 2.** SemWeaver architecture. A checker, patch and frozen validation scope
are combined with provenance-labelled evidence for bounded editing. Normal paired
execution supplies feedback and the retention decision. The latest attempt and
best accepted checker remain separate. Colors distinguish evidence/flow roles,
not correctness; optional traces and missing records remain explicit in the
underlying bundle. The native-only fixture guard is not a target oracle.

**Study design.** E1 compares three workflows on all 39 frozen CSA inputs: 15 are
initially noisy and 24 initially silent. KNighter's no-report policy performs no
edit on the 24 silent subjects; the noisy-15 view reuses E1 outputs. E3 uses the
same original 12-subject native/control subset, three decodes per arm and a
32-reply ceiling; decode 1 reuses E1 (24 cells), and decodes 2-3 add 48 cells.
E4 uses those 12 inputs with three native-only high-effort configurations, one
decode per model and a 2-reply ceiling (36 cells). E2 separately repeats one
CodeQL query three times, without a KNighter comparison.

The asterisk after the E1 ceiling refers to audited natural-stop reuses that
retain their original caps. Shared maximum replies do not imply matched realized
calls or tokens. The cohort is selected rather than random; repeated decodes are
not independent bugs, and E4 is not a cross-model native/control ablation.
Keep these details in the caption/protocol rather than adding tiny paragraphs
inside the diagram. The study figure is ready for manual reproduction but has
not been inserted into the manuscript; integration still requires pagination.

## Rebuild

Install `reportlab==4.4.4` and `python-pptx==1.0.2`, and supply Liberation Sans
regular/bold TTFs with `--font-dir` if they are not in the default Linux location.
Run `python3 build_figures.py --out .`. The script exports PDF, editable SVG,
PowerPoint and primitive JSON; PNG previews are rendered from the PDFs at 5x.
No model service, credentials or experimental execution is required.
