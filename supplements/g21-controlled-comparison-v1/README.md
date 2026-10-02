# G21 motivating-example controlled comparison

This package accompanies SemWeaver's G21 motivating example. The original
Linux pair comes from commit `97cba232549b`; the 114 additional inputs here
are small, constructed C programs based on its look-ahead access pattern.
They vary names, capacities, unrelated statements, and equivalent loop and
guard forms. They are **development tests**, not 114 independent Linux bugs or
unseen-project revisions. Some supplied feedback used these tests to improve
the illustrative checker.

KNighter's own report-driven loop took its recorded `No-FP` stop on G21 and
retained the original checker unchanged. Independent paired Linux replay still
observed one report on each revision. The three frozen checker artifacts were
then scanned, unedited, on the same 114 C inputs:

| Checker | Unsafe cases warned on (of 58) | Safe cases silent on (of 56) | Total correct |
| --- | ---: | ---: | ---: |
| KNighter retained checker | 31 | 25 | 56/114 |
| One-response SemWeaver checker scored in the 39-case study | 31 | 43 | 74/114 |
| Separately developed 14-response illustration | 58 | 56 | 114/114 |

The 14-response illustration still yields `1/0` on the original Linux pair,
as does the one-response main-study checker. Its additional fixture-guided
edits must not replace the main-study result or be used to claim unseen
generalization. This comparison describes behavior on the supplied source
forms, not semantic correctness on arbitrary programs.

## Offline reanalysis

From this directory, run `python3 verify_recorded.py .`. It checks exported
file hashes, every fixture's expected label, execution status, checker hash,
and the aggregate comparison. It needs only Python 3.10+ standard library;
there are no model, analyzer, compiler or network calls. `evidence/` contains
per-fixture outcomes and raw diagnostic logs. `inputs/` contains all 114 C
sources and suite manifests. `BASELINE_LOOP_PROVENANCE.json` gives the exact
KNighter stop and checker identity. `EXPORT_MANIFEST.json` distinguishes the
public text hashes from hashes of the frozen private originals.

Release documentation is English; original tool output remains unmodified
except for private path replacement. No credentials or project-wide license
are supplied. Third-party notices in the parent artifact remain applicable.
