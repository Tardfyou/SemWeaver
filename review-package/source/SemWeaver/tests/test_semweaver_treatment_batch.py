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
    / "run_semweaver_treatment_batch.py"
)
SPEC = importlib.util.spec_from_file_location("run_semweaver_treatment_batch", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC and SPEC.loader
SPEC.loader.exec_module(MODULE)


def test_classify_outcome_keeps_validity_loss_separate():
    treatment = {"success": True}
    validation = {
        "execution_valid": True,
        "pds": False,
        "vulnerable_hit": False,
        "fixed_silent": True,
    }
    assert MODULE.classify_outcome(treatment, validation) == "validity_loss"


def test_classify_outcome_does_not_promote_compile_failure():
    treatment = {
        "success": False,
        "candidate_changed": True,
        "compile_attempts": 1,
    }
    assert MODULE.classify_outcome(treatment, None) == "candidate_compile_failure"


def test_classify_outcome_names_external_candidate_compile_failure():
    treatment = {"success": True}
    validation = {"execution_valid": False, "build_return_code": 1, "pds": False}
    assert MODULE.classify_outcome(treatment, validation) == "candidate_compile_failure_external"


def test_classify_outcome_accepts_only_external_pds():
    treatment = {"success": True}
    validation = {
        "execution_valid": True,
        "pds": True,
        "vulnerable_hit": True,
        "fixed_silent": True,
    }
    assert MODULE.classify_outcome(treatment, validation) == "refined_pds"


def test_classify_outcome_preserves_preflight_rejection():
    treatment = {
        "success": False,
        "candidate_changed": False,
        "compile_attempts": 0,
        "failure_type": "preflight_candidate_failure",
    }
    assert MODULE.classify_outcome(treatment, None) == "preflight_candidate_failure"


def test_provider_retry_interrupts_formal_treatment_batch(tmp_path):
    events = tmp_path / "run_events.jsonl"
    events.write_text(
        "\n".join(json.dumps({"event": name}) for name in (
            "decision_started",
            "model_rate_limit_retry",
            "model_bind_fallback",
        )) + "\n",
        encoding="utf-8",
    )

    assert MODULE.provider_interruption_events(events) == [
        "model_rate_limit_retry",
        "model_bind_fallback",
    ]
    assert MODULE.provider_interruption_events(tmp_path / "missing.jsonl") == []


@pytest.mark.parametrize(
    "return_code,provider_event,reason",
    [
        (75, "", "model_rate_limit_exhausted"),
        (0, "model_rate_limit_retry", "provider_rate_limit_or_response_format_drift"),
    ],
)
@pytest.mark.parametrize("evidence_mode", ["native", "no_internal"])
def test_429_stops_batch_without_method_row(tmp_path, monkeypatch, return_code, provider_event, reason, evidence_mode):
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
    (cases / case_id / "csa" / "SAGenTestChecker.cpp").write_text("checker\n", encoding="utf-8")
    (evidence / case_id / "csa" / "evidence_bundle.json").write_text("{}\n", encoding="utf-8")
    (evidence / "BATCH_MANIFEST.json").write_text("{}\n", encoding="utf-8")
    (evidence / "BATCH_RESULT.json").write_text(
        json.dumps({"rows": [{"case_id": case_id, "eligible": True}]}) + "\n",
        encoding="utf-8",
    )
    binding.write_text(
        json.dumps({
            "passed": True,
            "batch_manifest_sha256": MODULE.sha256(evidence / "BATCH_MANIFEST.json"),
            "batch_result_sha256": MODULE.sha256(evidence / "BATCH_RESULT.json"),
        }) + "\n",
        encoding="utf-8",
    )
    baseline.write_text(
        json.dumps({
            "model": "test-model",
            "wire_api": "responses",
            "reasoning_effort": "medium",
            "rows": [{"case_id": case_id, "llm_calls": 1}],
        }) + "\n",
        encoding="utf-8",
    )
    config.write_text("llm: {}\n", encoding="utf-8")
    monkeypatch.setattr("sys.argv", [
        "batch", "--config", str(config), "--cases-root", str(cases),
        "--evidence-batch", str(evidence), "--binding-audit", str(binding),
        "--baseline-summary", str(baseline), "--linux-dir", str(linux),
        "--output-dir", str(output), "--backend-root", str(backend),
        "--evidence-mode", evidence_mode,
    ])

    def fake_run(command, **_kwargs):
        if command[:3] == ["git", "rev-parse", "HEAD"]:
            return SimpleNamespace(stdout="test-revision\n", returncode=0)
        if provider_event:
            events_path = output / case_id / "run_events.jsonl"
            events_path.write_text(
                json.dumps({"event": provider_event}) + "\n", encoding="utf-8"
            )
        assert command[command.index("--evidence-mode") + 1] == evidence_mode
        return SimpleNamespace(stdout="", stderr="rate limit exhausted", returncode=return_code)

    with patch.object(MODULE.subprocess, "run", side_effect=fake_run):
        assert MODULE.main() == 75

    assert not (output / "BATCH_RESULT.json").exists()
    interruption = json.loads((output / "BATCH_INTERRUPTED.json").read_text(encoding="utf-8"))
    assert interruption["status"] == "batch_interrupted_before_method_outcome"
    assert interruption["reason"] == reason
    assert interruption["completed_records"] == 0
    manifest = json.loads((output / "BATCH_MANIFEST.json").read_text(encoding="utf-8"))
    assert manifest["evidence_mode"] == evidence_mode
    assert ("ablation" in manifest["method"]) == (evidence_mode == "no_internal")
