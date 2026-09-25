import importlib.util
from pathlib import Path


SCRIPT = (
    Path(__file__).resolve().parents[1]
    / "experiments"
    / "knighter"
    / "experiment"
    / "audit_csa_evidence_binding.py"
)
SPEC = importlib.util.spec_from_file_location("audit_csa_evidence_binding", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC and SPEC.loader
SPEC.loader.exec_module(MODULE)


def test_patch_scope_uses_hunk_functions(tmp_path):
    patch = tmp_path / "patch.diff"
    patch.write_text(
        """\
diff --git a/a.c b/a.c
--- a/a.c
+++ b/a.c
@@ -10,1 +10,1 @@ static int first_target(void)
-old();
+new();
@@ -20,1 +20,1 @@ second_target(struct item *item)
-old2();
+new2();
"""
    )
    files, functions = MODULE.patch_scope(patch)
    assert files == {"a.c"}
    assert functions == {"a.c": {"first_target", "second_target"}}


def test_resolve_recorded_work_path(tmp_path):
    assert MODULE.resolve_recorded_path("/work/a/b.txt", tmp_path) == tmp_path / "a/b.txt"
