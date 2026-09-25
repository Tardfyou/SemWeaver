# Case 19 patch-local robustness probe

This suite was designed **after inspecting** the rep02 native PDS candidate.
It is a development-informed diagnostic, not a held-out generalization test
or an additional PDS numerator. The first three positive fixtures preserve
the cleanup relationship while varying an unrelated statement or enclosing
function name. `positive_rename_callback` tests a stronger semantic-preserving
rename and is expected to reveal whether the candidate hard-codes the
specific callback name. The two negatives remove the redundant cleanup or
change its object. Every result must be reported, including missed positives.

Run with `experiments/robustness/run_csa_metamorphic_suite.py`, the frozen
rep02 native case 19 selected checker, and a fresh output directory. The
runner hashes the checker, fixture manifest, and each source file before
building and scanning; it uses no model API or Linux checkout.
