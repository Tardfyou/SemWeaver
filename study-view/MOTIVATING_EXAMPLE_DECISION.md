# Motivating-example provenance and verification

Retain the previously model-produced, capacity-aware G21 checker as the
illustrative example, with its actual fixture-guided sequence and budget stated.
Do not replace the selected main-study checker or its scored result with it.

## Selection rationale rechecked on 2026-09-29

G21 remains the strongest supported **mechanism illustration** among the
reviewed candidates, rather than an assertion that it is the largest native
ablation win. The selection criteria are a clear vulnerability/patch relation,
an inspectable automatic code-edit lineage, no patch-name or line allowlist,
and recorded perturbations that test both warning recovery and suppression.
Its capacity/index relation explains why the original checker warns on safe
code and what the automatic edit adds without changing checker architecture.

The six favorable main comparisons were considered alongside it. G19 has
the most repeatable native warning outcome (15/0 in all three decodes), but
the recorded edit recognizes an additional release wrapper and control also
reaches 15/0 in one decode. It is suitable as a supplementary API-recognition
case, not a universal native-evidence advantage or fifteen separate bugs.
G24's report reduction lacks target-retention adjudication and does not recur
in later comparisons. G12 carries 73 fixed-side reports. G09, G25 and G35
recover warnings with residual fixed-side noise and lack G21's recorded
capacity/guard perturbation coverage. None is promoted merely for a favorable
single contrast.

The current recheck verified every replay-manifest input digest (123 for the
illustration and 122 for the main candidate), every suite-result digest,
normal fixture return codes, selected-checker identity and the recorded
14-response lineage. It made no model calls or checker edits. The source
still uses AST type, induction-variable and capacity relations; finite fixture
success is not a proof against every possible form of overfitting.

The original three-panel layout is retained: patch/extent on the left,
starting checker at upper right, and automatic refinement at lower right.
The standalone caption must identify the separate 14-response budget and
fixture feedback. G21 motivates the refinement task; comparative internal
evidence claims remain grounded in the full E1/E3 results, because both main
arms obtain 1/0 on G21.

| Version | Checker SHA-256 prefix | Original Linux pair | Recorded model responses | Core | Boundary | Control-flow | Renamed/capacity variants |
|---|---|---|---:|---:|---:|---:|---:|
| Current main-study candidate | `648d83fdb4db` | 1/0 | 1 | 28/28 | 2/6 | 11/20 | 33/60 |
| Retained automatic illustration | `5e953d86c9e4` | 1/0 | 14 | 28/28 | 6/6 | 20/20 | 60/60 |

Both versions were independently replayed, unmodified, on the same retained
114 fixtures with the measured Clang18 image and current fixture runner.
Every fixture execution completed normally. The current main checker passes
31/58 unsafe and 43/56 safe fixtures, not 114/114. Its broader guard/loop
limitations must remain visible in the artifact; a successful Linux pair is
not a proof of generalization.

The illustration's earlier generation/refinement sequence used two
GLM-5.3-Flash responses followed by twelve GPT-6-Luna-high responses. Checker
edits were model-produced. Researchers prepared fixtures and provided their
diagnostics during the additional refinement sequence. These facts are not
erased or relabeled as the one-response main execution. The replay performed
here made zero additional model calls and provided no further model feedback.

The illustration passes 58/58 unsafe and 56/56 safe fixture executions. This
supports the tested identifier renaming, independent-statement insertion,
constant capacities and represented unit-step loop/guard forms. It does not
establish universal absence of overfitting, unseen-project performance, general
alias reasoning or arbitrary dynamic extents. Reusing these fixtures after
feedback is not an independent unseen-data test.

The figure may show the actual array-extent/look-ahead relation and retained
AST-based architecture, using the original three-panel layout. Its caption
must identify a separately budgeted automatic illustrative sequence. It must
not imply that the main method's one-response checker passes all 114 probes or
that G21 alone proves an advantage from native evidence. Main G21 also reaches
1/0 without internal evidence.

Evidence locations:

- `motivating-current-main/`: current selected checker replay, including failures.
- `motivating-retained-automatic/`: fresh replay of the retained model-produced illustration.
- `review-revision-20260927/FINAL_SELECTION.json`: original generation/refinement lineage and Linux outcome bindings, retained outside this staging directory.

Both checker identities, all four suite manifests/fixtures, the runner and
replay adapters are hash-bound in the respective replay manifests. Neither
illustrative checker selection nor fixture results enter the frozen39 main
numerators or alter its model-call accounting.
