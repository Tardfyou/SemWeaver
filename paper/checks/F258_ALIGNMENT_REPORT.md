# Current data displays and author figure sources

The 2026-09-30 follow-up is documented in
`FINAL_FORMAT_AND_EVIDENCE_AUDIT.md`: all five tables and the numerical matrix
were checked against the reference PDF. Text-column wrapping and numeric
anchors were improved; the matrix now uses 8.369 bp text at its natural
378 bp width. Final placement is Table 1 p7, Table 2 p8, Table 3 p13,
Table 4 p14, and Figure 4/Table 5 p15. All underlying values are unchanged.

The active non-data drawings are the author's Mac PowerPoint exports under
`figures/manual/`. They are not generated from the former diagram styles.
The original three-page export and editable PPTX subset are retained there;
vector crops change page boxes only and leave each content stream unchanged.
The previous generated motivating, architecture and study-map assets and their
builders have been removed from the active manuscript and public paper tree.

Numerical outcomes are unchanged: five tables (six tabular panels) and the E3
12-by-3 paired-outcome matrix. Table fonts remain Nimbus Roman 8.369 bp with
0.33432 bp rules; double-rule spacing and the defined five-decimal RGB tokens
remain in `f258-table-style.tex`. F1 shading describes the documented 1-F1 bins,
not significance. E3 cell signs/colors denote recorded comparative directions,
not a performance ranking inferred from colors. All 36 pairs remain displayed.

Retained reproducibility files are `build_e3_repeat_figure.py`,
`e3-repeats-standalone.tex`, `fig-e3-repeats.tex/.json/.pdf/.png`, and the two
shared data-display style files. Main39, noisy15, evidence-origin and model
outcomes are unchanged. The author's study-design image is Figure 3 in Subjects;
the numerical E3 matrix remains as Figure 4. The former operational-definition
table is merged into Metrics prose without removing its definitions.

Current figure authority and instructions: `figures/manual/README.md`,
`IMPORT_MANIFEST.json` and `EXPORT_MANIFEST.json`. The Mac export is used directly
because an earlier LibreOffice render substituted missing fonts and reflowed
text. That failed rendering is not used in the paper. Original manuscript
backups, Git history and sealed experimental snapshots remain protected.
