from __future__ import annotations

import importlib.util
from pathlib import Path

import pytest

from src.refine.agent import LangChainRefinementAgent
from src.refine.models import RefinementRequest


SCRIPT = (
    Path(__file__).resolve().parents[1]
    / "experiments" / "knighter" / "experiment" / "run_semweaver_treatment_case.py"
)
SPEC = importlib.util.spec_from_file_location("run_semweaver_treatment_case", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC and SPEC.loader
SPEC.loader.exec_module(MODULE)


def test_target_hit_recovery_has_distinct_method_identity():
    assert MODULE.treatment_method("native", "target_hit_recovery") == (
        "semweaver_analyzer_evidence_csa_target_hit_recovery"
    )
    assert MODULE.treatment_method("no_internal", "target_hit_recovery") == (
        "semweaver_no_internal_csa_target_hit_recovery"
    )
    assert MODULE.treatment_method("native", "precision_refine") == (
        "semweaver_provenance_gated_csa_treatment"
    )


def test_target_hit_recovery_prompt_requires_both_paired_sides():
    agent = LangChainRefinementAgent(config={}, llm_override=object())
    request = RefinementRequest(
        analyzer="csa", patch_path="/tmp/fix.patch", work_dir="/tmp/work",
        target_path="/tmp/work/checker.cpp", objective="target_hit_recovery",
    )

    prompt = agent._render_task_prompt(request)

    assert "starting checker misses" in prompt
    assert "fixed revision silent" in prompt
    assert "unchanged checker" in prompt


def test_initial_target_miss_must_be_execution_valid_and_hash_bound():
    valid = {
        "execution_valid": True, "vulnerable_hit": False,
        "candidate_sha256": "starting",
    }
    MODULE.validate_target_recovery_feedback(
        valid, is_continuation=False, starting_checker_sha256="starting"
    )
    with pytest.raises(ValueError, match="valid starting"):
        MODULE.validate_target_recovery_feedback(
            {**valid, "execution_valid": False},
            is_continuation=False, starting_checker_sha256="starting",
        )
    with pytest.raises(ValueError, match="starting vulnerable-side miss"):
        MODULE.validate_target_recovery_feedback(
            {**valid, "vulnerable_hit": True},
            is_continuation=False, starting_checker_sha256="starting",
        )


def test_invalid_later_candidate_is_repair_feedback_not_starting_error():
    invalid = {
        "execution_valid": False, "candidate_sha256": "changed",
        "vulnerable_alerts": -10, "fixed_alerts": -10,
        "vulnerable_object_counts": {"target.o": -10},
        "fixed_object_counts": {"target.o": -10},
    }
    MODULE.validate_target_recovery_feedback(
        invalid, is_continuation=True, starting_checker_sha256="changed"
    )
    summary = MODULE.target_recovery_feedback_summary(invalid)
    assert "execution-invalid, not a target miss" in summary
    assert "vulnerable_alerts=-10" not in summary
    with pytest.raises(ValueError, match="not bound"):
        MODULE.validate_target_recovery_feedback(
            invalid, is_continuation=True, starting_checker_sha256="different"
        )
    with pytest.raises(ValueError, match="PDS candidate"):
        MODULE.validate_target_recovery_feedback(
            {**invalid, "pds": True},
            is_continuation=True, starting_checker_sha256="changed",
        )
