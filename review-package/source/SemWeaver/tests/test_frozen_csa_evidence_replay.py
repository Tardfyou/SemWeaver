import importlib.util
from pathlib import Path


SCRIPT = (
    Path(__file__).resolve().parents[1]
    / "experiments"
    / "knighter"
    / "experiment"
    / "collect_frozen_csa_evidence.py"
)
SPEC = importlib.util.spec_from_file_location("collect_frozen_csa_evidence", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC and SPEC.loader
SPEC.loader.exec_module(MODULE)


def test_clean_frozen_plan_removes_all_stale_evidence():
    raw = {
        "files_changed": ["driver.c"],
        "evidence_bundle": {"records": ["top-level-stale"]},
        "patchweaver": {
            "summary": "keep",
            "evidence_bundle": {"records": ["stale"]},
            "refinement_evidence_bundles": {"csa": {"records": ["manual"]}},
            "validation_feedback": [{"manual": True}],
            "validation_feedback_history": [{"manual": True}],
        },
    }
    cleaned = MODULE.clean_frozen_plan(raw)
    assert cleaned["files_changed"] == ["driver.c"]
    assert cleaned["patchweaver"] == {"summary": "keep"}
    assert "evidence_bundle" not in cleaned
    assert raw["patchweaver"]["evidence_bundle"]["records"] == ["stale"]


def test_origin_counts_reads_record_provenance_only():
    bundle = {
        "records": [
            {"analyzer": "csa", "provenance": {"origin": "source_derived"}},
            {"analyzer": "patch", "provenance": {"origin": "source_derived"}},
            {"analyzer": "csa", "provenance": {"origin": "analyzer_internal"}},
            {"analyzer": "csa", "provenance": {}},
        ]
    }
    assert MODULE.origin_counts(bundle) == {
        "analyzer_internal": 1,
        "source_derived": 2,
        "unknown": 1,
    }
