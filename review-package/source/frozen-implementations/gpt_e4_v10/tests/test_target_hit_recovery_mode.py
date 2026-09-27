from __future__ import annotations

import importlib.util
from pathlib import Path

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
