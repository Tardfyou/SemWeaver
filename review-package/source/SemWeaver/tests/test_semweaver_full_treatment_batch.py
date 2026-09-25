import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

import pytest


SCRIPT = (
    Path(__file__).resolve().parents[1]
    / "experiments"
    / "knighter"
    / "experiment"
    / "run_semweaver_full_treatment_batch.py"
)
SPEC = importlib.util.spec_from_file_location("run_semweaver_full_treatment_batch", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC and SPEC.loader
SPEC.loader.exec_module(MODULE)


def test_full_budget_classification_requires_external_pds():
    treatment = {"success": True}
    residual = {
        "execution_valid": True,
        "pds": False,
        "vulnerable_hit": True,
        "fixed_silent": False,
    }
    accepted = {
        "execution_valid": True,
        "pds": True,
        "vulnerable_hit": True,
        "fixed_silent": True,
    }
    assert MODULE.classify_outcome(treatment, residual) == "residual_fixed_noise"
    assert MODULE.classify_outcome(treatment, accepted) == "refined_pds"


def test_failed_attempt_can_continue_unchanged_upstream_candidate():
    treatment = {
        "success": False,
        "candidate_changed": False,
        "candidate_sha256": "abc",
        "error_message": "model response parse failure",
    }
    assert MODULE.can_continue_failed_candidate(
        treatment, manifest_exists=True, candidate_exists=True
    )


def test_selection_rolls_back_to_last_externally_validated_candidate():
    attempts = [
        {"attempt": 1, "outcome": "residual_fixed_noise", "validation_result_sha256": "ok"},
        {"attempt": 2, "outcome": "candidate_compile_failure", "validation_result_sha256": ""},
    ]
    assert MODULE.selected_outcome(attempts) == ("residual_fixed_noise", 1)


def test_matched_caps_bind_every_case_and_total():
    baseline = {
        "subjects": 2,
        "unscored": 0,
        "baseline_model_calls": 5,
        "rows": [
            {"case_id": "a", "llm_calls": 1},
            {"case_id": "b", "llm_calls": 4},
        ],
    }
    assert MODULE.matched_call_budgets(baseline, ["a", "b"]) == {"a": 1, "b": 4}
    with pytest.raises(ValueError, match="denominator drift"):
        MODULE.matched_call_budgets({**baseline, "baseline_model_calls": 6}, ["a", "b"])
    with pytest.raises(ValueError, match="case identities"):
        MODULE.matched_call_budgets(baseline, ["a", "c"])


def test_feedback_profile_detects_any_provider_retry_or_format_fallback(tmp_path: Path):
    events = tmp_path / "run_events.jsonl"
    events.write_text("\n".join(json.dumps({"event": name}) for name in (
        "decision_started", "model_rate_limit_retry", "model_bind_fallback"
    )) + "\n", encoding="utf-8")
    assert MODULE.provider_interruption_events(events) == [
        "model_rate_limit_retry", "model_bind_fallback"
    ]
    assert MODULE.rate_limit_observed("Error code: 429 - gateway_concurrency_limit")
    assert not MODULE.rate_limit_observed("candidate_compile_failure")


@pytest.mark.parametrize("evidence_mode", ["native", "no_internal"])
def test_matched_feedback_validates_each_candidate_before_next_model_call(
    tmp_path: Path, monkeypatch, evidence_mode: str
):
    case_id = "case_a"
    cases = tmp_path / "cases"
    evidence = tmp_path / "evidence"
    output = tmp_path / "output"
    backend = tmp_path / "backend"
    linux = tmp_path / "linux"
    config = tmp_path / "config.yaml"
    baseline = tmp_path / "baseline.json"
    binding = tmp_path / "binding.json"
    for directory in (
        cases / case_id / "patches",
        cases / case_id / "csa",
        evidence / case_id / "csa",
        linux,
    ):
        directory.mkdir(parents=True)
    (cases / case_id / "patches" / "commit.patch").write_text("patch\n", encoding="utf-8")
    (cases / case_id / "csa" / "SAGenTestChecker.cpp").write_text("start\n", encoding="utf-8")
    (evidence / case_id / "csa" / "evidence_bundle.json").write_text("{}\n", encoding="utf-8")
    (evidence / "BATCH_MANIFEST.json").write_text("{}\n", encoding="utf-8")
    (evidence / "BATCH_RESULT.json").write_text(
        json.dumps({"rows": [{"case_id": case_id, "eligible": True}]}) + "\n",
        encoding="utf-8",
    )
    binding.write_text(json.dumps({
        "passed": True,
        "batch_manifest_sha256": MODULE.sha256(evidence / "BATCH_MANIFEST.json"),
        "batch_result_sha256": MODULE.sha256(evidence / "BATCH_RESULT.json"),
    }) + "\n", encoding="utf-8")
    baseline.write_text(json.dumps({
        "subjects": 1,
        "unscored": 0,
        "baseline_model_calls": 2,
        "model": "test-model",
        "wire_api": "responses",
        "reasoning_effort": "high",
        "rows": [{"case_id": case_id, "llm_calls": 2}],
    }) + "\n", encoding="utf-8")
    config.write_text("llm: {}\n", encoding="utf-8")
    monkeypatch.setattr("sys.argv", [
        "batch", "--config", str(config), "--cases-root", str(cases),
        "--evidence-batch", str(evidence), "--binding-audit", str(binding),
        "--baseline-summary", str(baseline), "--linux-dir", str(linux),
        "--output-dir", str(output), "--backend-root", str(backend),
        "--attempt-cap", "1", "--evidence-mode", evidence_mode,
    ])

    order: list[str] = []

    def fake_run(command, **_kwargs):
        if command[:3] == ["git", "rev-parse", "HEAD"]:
            return SimpleNamespace(stdout="test-revision\n", returncode=0)
        attempt_output = Path(command[command.index("--output-dir") + 1])
        attempt_output.mkdir(parents=True, exist_ok=True)
        if "run_semweaver_treatment_case.py" in str(command[1]):
            number = len([item for item in order if item == "treatment"]) + 1
            assert command[command.index("--max-iterations") + 1] == "1"
            assert command[command.index("--evidence-mode") + 1] == evidence_mode
            if number == 2:
                prior = output / case_id / "attempt-01"
                assert command[command.index("--feedback-result") + 1] == str(
                    prior / "frozen_validation" / "RESULT.json"
                )
                assert command[command.index("--starting-candidate") + 1] == str(
                    prior / "SAGenTestChecker.cpp"
                )
            candidate = attempt_output / "SAGenTestChecker.cpp"
            candidate.write_text(f"candidate-{number}\n", encoding="utf-8")
            (attempt_output / "RUN_MANIFEST.json").write_text(json.dumps({
                "success": True,
                "candidate_changed": True,
                "candidate_sha256": MODULE.sha256(candidate),
                "model": "test-model",
                "wire_api": "responses",
                "reasoning_effort": "high",
                "llm_usage": {"call_count": 1},
            }) + "\n", encoding="utf-8")
            order.append("treatment")
        else:
            assert "validate_frozen_csa_candidate.py" in str(command[1])
            number = len([item for item in order if item == "validation"]) + 1
            (attempt_output / "RESULT.json").write_text(json.dumps({
                "execution_valid": True,
                "pds": number == 2,
                "vulnerable_hit": True,
                "fixed_silent": number == 2,
                "vulnerable_alerts": 1,
                "fixed_alerts": 0 if number == 2 else 1,
            }) + "\n", encoding="utf-8")
            order.append("validation")
        return SimpleNamespace(stdout="", stderr="", returncode=0)

    with patch.object(MODULE.subprocess, "run", side_effect=fake_run):
        assert MODULE.main() == 0
    assert order == ["treatment", "validation", "treatment", "validation"]
    result = json.loads((output / "BATCH_RESULT.json").read_text(encoding="utf-8"))
    assert result["accepted_pds"] == 1
    assert result["llm_calls"] == 2
    assert json.loads((output / "BATCH_MANIFEST.json").read_text())["total_model_call_cap"] == 2
    assert result["rows"][0]["attempts"] == 2
    assert result["paired_validation_seconds"] >= 0
    assert result["elapsed_seconds"] >= result["paired_validation_seconds"]


def test_feedback_batch_429_is_interrupted_not_a_method_failure(tmp_path: Path, monkeypatch):
    case_id = "case_a"
    cases = tmp_path / "cases"
    evidence = tmp_path / "evidence"
    output = tmp_path / "output"
    linux = tmp_path / "linux"
    for directory in (cases / case_id / "patches", cases / case_id / "csa",
                      evidence / case_id / "csa", linux):
        directory.mkdir(parents=True)
    (cases / case_id / "patches" / "commit.patch").write_text("patch\n")
    (cases / case_id / "csa" / "SAGenTestChecker.cpp").write_text("checker\n")
    (evidence / case_id / "csa" / "evidence_bundle.json").write_text("{}\n")
    (evidence / "BATCH_MANIFEST.json").write_text("{}\n")
    (evidence / "BATCH_RESULT.json").write_text(json.dumps({
        "rows": [{"case_id": case_id, "eligible": True}]
    }))
    binding = tmp_path / "binding.json"
    binding.write_text(json.dumps({
        "passed": True,
        "batch_manifest_sha256": MODULE.sha256(evidence / "BATCH_MANIFEST.json"),
        "batch_result_sha256": MODULE.sha256(evidence / "BATCH_RESULT.json"),
    }))
    baseline = tmp_path / "baseline.json"
    baseline.write_text(json.dumps({
        "subjects": 1, "unscored": 0, "baseline_model_calls": 1,
        "model": "test-model", "wire_api": "responses", "reasoning_effort": "high",
        "rows": [{"case_id": case_id, "llm_calls": 1}],
    }))
    config = tmp_path / "config.yaml"
    config.write_text("llm: {}\n")
    monkeypatch.setattr("sys.argv", [
        "batch", "--config", str(config), "--cases-root", str(cases),
        "--evidence-batch", str(evidence), "--binding-audit", str(binding),
        "--baseline-summary", str(baseline), "--linux-dir", str(linux),
        "--output-dir", str(output), "--backend-root", str(tmp_path / "backend"),
        "--attempt-cap", "1",
    ])

    def fake_run(command, **_kwargs):
        if command[:3] == ["git", "rev-parse", "HEAD"]:
            return SimpleNamespace(stdout="test-revision\n", returncode=0)
        attempt_output = Path(command[command.index("--output-dir") + 1])
        attempt_output.mkdir(parents=True, exist_ok=True)
        (attempt_output / "run_events.jsonl").write_text(
            json.dumps({"event": "model_rate_limit_retry"}) + "\n",
            encoding="utf-8",
        )
        return SimpleNamespace(stdout="", stderr="", returncode=0)

    with patch.object(MODULE.subprocess, "run", side_effect=fake_run):
        assert MODULE.main() == 75
    assert not (output / "BATCH_RESULT.json").exists()
    interrupted = json.loads((output / "BATCH_INTERRUPTED.json").read_text())
    assert interrupted["reason"] == "provider_rate_limit_or_binding_fallback"
    assert interrupted["completed_cases"] == 0


def test_matched_feedback_can_repair_hash_bound_lsp_failure(tmp_path: Path, monkeypatch):
    case_id = "case_a"
    cases = tmp_path / "cases"
    evidence = tmp_path / "evidence"
    output = tmp_path / "output"
    linux = tmp_path / "linux"
    for directory in (cases / case_id / "patches", cases / case_id / "csa",
                      evidence / case_id / "csa", linux):
        directory.mkdir(parents=True)
    (cases / case_id / "patches" / "commit.patch").write_text("patch\n")
    (cases / case_id / "csa" / "SAGenTestChecker.cpp").write_text("start\n")
    (evidence / case_id / "csa" / "evidence_bundle.json").write_text("{}\n")
    (evidence / "BATCH_MANIFEST.json").write_text("{}\n")
    (evidence / "BATCH_RESULT.json").write_text(json.dumps({
        "rows": [{"case_id": case_id, "eligible": True}]
    }))
    binding = tmp_path / "binding.json"
    binding.write_text(json.dumps({
        "passed": True,
        "batch_manifest_sha256": MODULE.sha256(evidence / "BATCH_MANIFEST.json"),
        "batch_result_sha256": MODULE.sha256(evidence / "BATCH_RESULT.json"),
    }))
    baseline = tmp_path / "baseline.json"
    baseline.write_text(json.dumps({
        "subjects": 1, "unscored": 0, "baseline_model_calls": 2,
        "model": "test-model", "wire_api": "responses", "reasoning_effort": "high",
        "rows": [{"case_id": case_id, "llm_calls": 2}],
    }))
    config = tmp_path / "config.yaml"
    config.write_text("llm: {}\n")
    monkeypatch.setattr("sys.argv", [
        "batch", "--config", str(config), "--cases-root", str(cases),
        "--evidence-batch", str(evidence), "--binding-audit", str(binding),
        "--baseline-summary", str(baseline), "--linux-dir", str(linux),
        "--output-dir", str(output), "--backend-root", str(tmp_path / "backend"),
        "--attempt-cap", "1",
    ])

    order: list[str] = []

    def fake_run(command, **_kwargs):
        if command[:3] == ["git", "rev-parse", "HEAD"]:
            return SimpleNamespace(stdout="test-revision\n", returncode=0)
        attempt_output = Path(command[command.index("--output-dir") + 1])
        attempt_output.mkdir(parents=True, exist_ok=True)
        if "run_semweaver_treatment_case.py" in str(command[1]):
            number = len([event for event in order if event == "treatment"]) + 1
            candidate = attempt_output / "SAGenTestChecker.cpp"
            candidate.write_text(f"candidate-{number}\n")
            if number == 2:
                prior = output / case_id / "attempt-01"
                assert command[command.index("--feedback-run-manifest") + 1] == str(
                    prior / "RUN_MANIFEST.json"
                )
                assert command[command.index("--starting-candidate") + 1] == str(
                    prior / "SAGenTestChecker.cpp"
                )
            (attempt_output / "RUN_MANIFEST.json").write_text(json.dumps({
                "success": number == 2,
                "failure_type": "preflight_lsp_failure" if number == 1 else "",
                "candidate_changed": True,
                "candidate_sha256": MODULE.sha256(candidate),
                "model": "test-model", "wire_api": "responses",
                "reasoning_effort": "high", "llm_usage": {"call_count": 1},
            }))
            order.append("treatment")
        else:
            assert "validate_frozen_csa_candidate.py" in str(command[1])
            (attempt_output / "RESULT.json").write_text(json.dumps({
                "execution_valid": True, "pds": True,
                "vulnerable_hit": True, "fixed_silent": True,
                "vulnerable_alerts": 1, "fixed_alerts": 0,
            }))
            order.append("validation")
        return SimpleNamespace(stdout="", stderr="", returncode=0)

    with patch.object(MODULE.subprocess, "run", side_effect=fake_run):
        assert MODULE.main() == 0
    assert order == ["treatment", "treatment", "validation"]
    result = json.loads((output / "BATCH_RESULT.json").read_text())
    assert result["accepted_pds"] == 1 and result["llm_calls"] == 2
    assert result["rows"][0]["attempt_rows"][0]["outcome"] == "preflight_lsp_failure"
