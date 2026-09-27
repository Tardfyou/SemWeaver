import importlib.util
from pathlib import Path

import pytest

from src.tools.compile import CompileCheckerTool


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


def test_failed_vulnerable_scan_skips_unneeded_fixed_scan():
    assert MODULE.should_skip_fixed_side(-1)
    assert MODULE.should_skip_fixed_side(-999)
    assert not MODULE.should_skip_fixed_side(0)
    assert not MODULE.should_skip_fixed_side(2)


def test_inclusive_parent_lookup_self_loop_is_rejected_before_execution():
    candidate = """
    const ForStmt *FS = findSpecificTypeInParents<ForStmt>(ASE, C);
    while (FS) {
      if (satisfied(FS)) break;
      FS = findSpecificTypeInParents<ForStmt>(FS, C);
    }
    """
    assert MODULE.nonterminating_ancestor_walks(candidate) == [{
        "kind": "inclusive_parent_lookup_on_same_loop_cursor",
        "cursor": "FS", "lookup_type": "ForStmt", "line": 3,
    }]
    compile_result = CompileCheckerTool().execute(
        checker_name="SAGenTestChecker", source_code=candidate
    )
    assert not compile_result.success
    assert compile_result.metadata["failure_kind"] == "candidate_nontermination"
    assert "Advance to a distinct parent" in compile_result.error


def test_ancestor_lookup_on_distinct_parent_is_not_rejected():
    candidate = """
    while (FS) {
      auto parent = C.getLocationContext()->getParentMap().getParent(FS);
      FS = findSpecificTypeInParents<ForStmt>(parent, C);
    }
    """
    assert MODULE.nonterminating_ancestor_walks(candidate) == []


def test_scan_failure_reason_distinguishes_timeout_and_warning_limit():
    assert MODULE.scan_failure_kind(
        "Timeout of 1800 seconds exceeded! Build stopped."
    ) == "analyzer_timeout"
    assert MODULE.scan_failure_kind(
        "Warning limit of 300 exceeded! Build stopped."
    ) == "warning_volume_limit"


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
