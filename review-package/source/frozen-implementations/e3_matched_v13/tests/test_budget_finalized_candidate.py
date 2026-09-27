import importlib.util
from pathlib import Path


SCRIPT = (
    Path(__file__).resolve().parents[1]
    / "experiments" / "knighter" / "experiment"
    / "run_semweaver_treatment_case.py"
)
SPEC = importlib.util.spec_from_file_location("budget_finalized_case", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC and SPEC.loader
SPEC.loader.exec_module(MODULE)


def _event(tool_name: str, success: bool = True) -> dict:
    return {"event": "tool_result", "tool_name": tool_name, "success": success}


def test_compiled_reviewed_last_edit_is_scanned_at_call_cap(tmp_path: Path):
    plugin = tmp_path / "SAGenTestChecker.so"
    plugin.write_bytes(b"compiled plugin")
    events = [
        _event("apply_patch"), _event("compile_checker"), _event("review_artifact"),
    ]
    assert MODULE.budget_finalized_candidate_ready(
        error_message="Frozen model-call cap reached: 1/1",
        candidate_changed=True, compiled_plugin=plugin,
        events=events,
    )


def test_uncompiled_last_edit_cannot_use_stale_plugin(tmp_path: Path):
    plugin = tmp_path / "SAGenTestChecker.so"
    plugin.write_bytes(b"old plugin")
    events = [
        _event("apply_patch"), _event("compile_checker"),
        _event("review_artifact"), _event("apply_patch"),
    ]
    assert not MODULE.budget_finalized_candidate_ready(
        error_message="Frozen model-call cap reached: 2/2",
        candidate_changed=True, compiled_plugin=plugin,
        events=events,
    )


def test_provider_error_is_not_budget_finalized(tmp_path: Path):
    plugin = tmp_path / "SAGenTestChecker.so"
    plugin.write_bytes(b"compiled plugin")
    assert not MODULE.budget_finalized_candidate_ready(
        error_message="model_output_limit_exhausted",
        candidate_changed=True, compiled_plugin=plugin,
        events=[_event("apply_patch"), _event("compile_checker"), _event("review_artifact")],
    )


def test_budget_candidate_can_be_replayed_before_final_review(tmp_path: Path):
    plugin = tmp_path / "SAGenTestChecker.so"
    plugin.write_bytes(b"compiled plugin")
    assert MODULE.budget_finalized_candidate_ready(
        error_message="Frozen model-call cap reached: 1/1",
        candidate_changed=True, compiled_plugin=plugin,
        events=[_event("review_artifact"), _event("apply_patch"), _event("compile_checker")],
    )
