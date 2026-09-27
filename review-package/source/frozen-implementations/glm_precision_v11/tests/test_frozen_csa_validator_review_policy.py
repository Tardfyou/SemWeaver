import importlib.util
from pathlib import Path

import pytest


SCRIPT = (
    Path(__file__).resolve().parents[1]
    / "experiments" / "knighter" / "experiment"
    / "validate_frozen_csa_candidate.py"
)
SPEC = importlib.util.spec_from_file_location("validate_frozen_csa_candidate", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC and SPEC.loader
SPEC.loader.exec_module(MODULE)


def test_review_finding_does_not_hide_paired_execution_in_primary_oracle():
    scores = MODULE.score_candidate(
        execution_valid=True, vulnerable_alerts=2, fixed_alerts=0,
        review_passed=False, review_policy="diagnostic",
    )
    assert scores == {
        "vulnerable_hit": True,
        "fixed_silent": True,
        "pds_behavior": True,
        "pds": True,
        "adoptable_with_review_gate": False,
    }


def test_legacy_review_gate_remains_reproducible():
    scores = MODULE.score_candidate(
        execution_valid=True, vulnerable_alerts=2, fixed_alerts=0,
        review_passed=False, review_policy="gate",
    )
    assert scores["pds_behavior"] is True
    assert scores["pds"] is False


@pytest.mark.parametrize(
    "execution_valid,vulnerable_alerts,fixed_alerts",
    [(False, None, None), (True, 0, 0), (True, 2, 1)],
)
def test_no_pds_without_valid_hit_and_fixed_silence(
    execution_valid, vulnerable_alerts, fixed_alerts,
):
    scores = MODULE.score_candidate(
        execution_valid=execution_valid,
        vulnerable_alerts=vulnerable_alerts,
        fixed_alerts=fixed_alerts,
        review_passed=True, review_policy="diagnostic",
    )
    assert scores["pds"] is False
