import importlib.util
from pathlib import Path


SCRIPT = (
    Path(__file__).resolve().parents[1]
    / "experiments"
    / "knighter"
    / "experiment"
    / "run_semweaver_treatment_case.py"
)
SPEC = importlib.util.spec_from_file_location("run_semweaver_treatment_case", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC and SPEC.loader
SPEC.loader.exec_module(MODULE)


def test_patch_source_paths_are_bounded_and_deduplicated():
    patch = """\
diff --git a/drivers/a.c b/drivers/a.c
--- a/drivers/a.c
+++ b/drivers/a.c
--- a/drivers/a.c
--- a/../escape.c
--- /absolute.c
diff --git a/include/b.h b/include/b.h
--- a/include/b.h
+++ b/include/b.h
"""
    assert MODULE.patch_source_paths(patch) == ["drivers/a.c", "include/b.h"]
