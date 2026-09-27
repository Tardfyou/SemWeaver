import importlib.util
import json
from pathlib import Path

import pytest


SCRIPT = (
    Path(__file__).resolve().parents[1]
    / "experiments" / "knighter" / "experiment"
    / "summarize_generate_only_portfolio.py"
)
SPEC = importlib.util.spec_from_file_location("portfolio_summary", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC and SPEC.loader
SPEC.loader.exec_module(MODULE)


def _write(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value) + "\n", encoding="utf-8")


def test_complete_39_case_adopt_only_pds_counts(tmp_path: Path) -> None:
    screen_path = tmp_path / "SCREEN_RESULT.json"
    screen_rows = [
        {"case_id": f"P{i:02d}", "execution_valid": True,
         "vulnerable_alerts": 1, "fixed_alerts": 3 if i == 0 else 1}
        for i in range(14)
    ] + [
        {"case_id": f"R{i:02d}", "execution_valid": True,
         "vulnerable_alerts": 0, "fixed_alerts": 0}
        for i in range(25)
    ]
    _write(screen_path, {"requested_cases": 39, "completed_records": 39, "rows": screen_rows})
    directories = {name: tmp_path / name for name in ("precision", "recovery")}
    for name, directory in directories.items():
        cases = [row["case_id"] for row in screen_rows if row["case_id"].startswith(name[0].upper())]
        manifest_path = directory / "BATCH_MANIFEST.json"
        _write(manifest_path, {
            "method": name, "objective": "precision_refine" if name == "precision" else "target_hit_recovery",
            "execution_order": cases,
            "implementation_revision": "revision",
            "evidence_batch_manifest_sha256": "evidence-manifest",
            "evidence_batch_result_sha256": "evidence-result",
            "binding_audit_sha256": "binding",
            "evidence_mode": "native", "model": "model", "wire_api": "responses",
            "reasoning_effort": "high", "temperature": 0.0,
            "call_cap_per_subject": 8, "attempt_cap": 4,
            "max_tokens_per_call": 16384,
        })
        rows = []
        for case in cases:
            accepted = case == "R00"
            attempts = []
            if accepted or case in {"P00", "R01"}:
                validation_path = directory / case / "attempt-01" / "frozen_validation" / "RESULT.json"
                trial_vulnerable = 2 if case.startswith("R") else 1
                trial_fixed = 0 if accepted else (1 if case == "P00" else 2)
                _write(validation_path, {
                    "execution_valid": True, "pds": accepted, "candidate_sha256": "candidate",
                    "vulnerable_alerts": trial_vulnerable, "fixed_alerts": trial_fixed,
                })
                attempts = [{
                    "attempt": 1, "outcome": "refined_pds" if accepted else "residual_fixed_noise",
                    "candidate_sha256": "candidate",
                    "validation_result_sha256": MODULE.sha256(validation_path),
                }]
            rows.append({
                "case_id": case, "accepted_pds": accepted,
                "selected_attempt": 1 if accepted else None,
                "attempt_rows": attempts, "outcome": "refined_pds" if accepted else "no_candidate_within_budget",
                "llm_calls": 1,
            })
        _write(directory / "BATCH_RESULT.json", {
            "requested_cases": len(cases), "completed_records": len(cases),
            "batch_manifest_sha256": MODULE.sha256(manifest_path), "rows": rows,
        })

    result = MODULE.summarize(screen_path, directories["precision"], directories["recovery"])

    assert result["baseline"]["TP"] == 14
    assert result["baseline"]["FP"] == 14
    assert result["baseline"]["FN"] == 25
    assert result["post_adoption"]["TP"] == 15
    assert result["post_adoption"]["FP"] == 14
    assert result["post_adoption"]["FN"] == 24
    assert result["post_adoption"]["PDS"] == 1
    effects = result["diagnostic_candidate_effects"]
    assert effects["precision_cases_with_partial_nonzero_fixed_reduction"] == 1
    assert effects["precision_best_observed_fixed_alert_reduction_sum"] == 2
    assert effects["recovery_cases_with_any_vulnerable_hit"] == 2
    assert effects["recovery_cases_with_hit_but_residual_fixed_noise"] == 1

    recovery_manifest_path = directories["recovery"] / "BATCH_MANIFEST.json"
    recovery_manifest = json.loads(recovery_manifest_path.read_text())
    recovery_manifest["model"] = "different-model"
    _write(recovery_manifest_path, recovery_manifest)
    recovery_result_path = directories["recovery"] / "BATCH_RESULT.json"
    recovery_result = json.loads(recovery_result_path.read_text())
    recovery_result["batch_manifest_sha256"] = MODULE.sha256(recovery_manifest_path)
    _write(recovery_result_path, recovery_result)
    with pytest.raises(ValueError, match="frozen protocol"):
        MODULE.summarize(screen_path, directories["precision"], directories["recovery"])


def test_partial_screen_is_rejected(tmp_path: Path) -> None:
    screen_path = tmp_path / "SCREEN_RESULT.json"
    _write(screen_path, {"requested_cases": 39, "completed_records": 1, "rows": [{"case_id": "P00"}]})
    with pytest.raises(ValueError, match="complete"):
        MODULE.summarize(screen_path, tmp_path / "precision", tmp_path / "recovery")
