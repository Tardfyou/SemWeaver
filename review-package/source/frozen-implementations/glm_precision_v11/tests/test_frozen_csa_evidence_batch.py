import importlib.util
from pathlib import Path


SCRIPT = (
    Path(__file__).resolve().parents[1]
    / "experiments"
    / "knighter"
    / "experiment"
    / "collect_frozen_csa_evidence_batch.py"
)
SPEC = importlib.util.spec_from_file_location("collect_frozen_csa_evidence_batch", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC and SPEC.loader
SPEC.loader.exec_module(MODULE)


def test_cohort_case_ids_accepts_bound_entries_and_deduplicates():
    payload = {
        "cases": [
            {"case_id": "case-02", "sha256": "a"},
            "case-05",
            {"case_id": "case-02", "sha256": "a"},
            {},
        ]
    }
    assert MODULE.cohort_case_ids(payload) == ["case-02", "case-05"]
