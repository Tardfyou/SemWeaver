import importlib.util
import json
from pathlib import Path


SCRIPT = (
    Path(__file__).resolve().parents[1]
    / "experiments" / "knighter" / "experiment"
    / "summarize_knighter_full39_system.py"
)
SPEC = importlib.util.spec_from_file_location("knighter_full39_system", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC and SPEC.loader
SPEC.loader.exec_module(MODULE)


def _write(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload) + "\n", encoding="utf-8")


def test_full39_includes_no_report_target_misses_without_fake_failures(tmp_path: Path):
    screen_path = tmp_path / "SCREEN_RESULT.json"
    screen_rows = [
        {"case_id": f"P{i:02d}", "execution_valid": True,
         "vulnerable_alerts": 1, "fixed_alerts": 1,
         "checker_sha256": f"source-{i}"}
        for i in range(14)
    ] + [
        {"case_id": f"R{i:02d}", "execution_valid": True,
         "vulnerable_alerts": 0, "fixed_alerts": 0,
         "checker_sha256": f"miss-{i}"}
        for i in range(25)
    ]
    _write(screen_path, {"requested_cases": 39, "completed_records": 39, "rows": screen_rows})
    baseline_dir = tmp_path / "baseline"
    batch_path = baseline_dir / "BATCH_RESULT.json"
    _write(batch_path, {
        "requested_cases": 14, "execution_valid": 14,
        "cases": [{
            "case_id": f"P{i:02d}",
            "result": {"execution_valid": True, "model_calls_used": 1},
        } for i in range(14)],
    })
    strict_dir = tmp_path / "strict"
    strict_manifest_path = strict_dir / "STRICT_MANIFEST.json"
    _write(strict_manifest_path, {"baseline_batch_result_sha256": MODULE.sha256(batch_path)})
    validation_path = strict_dir / "P00" / "RESULT.json"
    _write(validation_path, {
        "execution_valid": True, "pds": True,
        "vulnerable_alerts": 1, "fixed_alerts": 0,
        "candidate_sha256": "refined",
    })
    _write(strict_dir / "STRICT_RESULT.json", {
        "subjects": 14, "unscored": 0,
        "strict_manifest_sha256": MODULE.sha256(strict_manifest_path),
        "rows": [{
            "case_id": f"P{i:02d}", "strict_pds": i == 0,
            "llm_calls": 1,
            "result_sha256": MODULE.sha256(validation_path) if i == 0 else "",
        } for i in range(14)],
    })
    no_report_dir = tmp_path / "no_report"
    _write(no_report_dir / "BATCH_RESULT.json", {
        "requested_cases": 25, "execution_valid": 25,
        "cases": [{
            "case_id": f"R{i:02d}",
            "result": {
                "execution_valid": True, "no_fixed_reports": True,
                "fixed_report_count_used": 0, "model_calls_used": 0,
                "checker_sha256": f"miss-{i}",
                "results": [{"result": "Perfect", "refined": False}],
            },
        } for i in range(25)],
    })

    result = MODULE.summarize(screen_path, baseline_dir, strict_dir, no_report_dir)

    assert result["subjects"] == 39
    assert result["report_refinement_cases"] == 14
    assert result["no_report_no_op_cases"] == 25
    assert result["baseline"]["FP"] == 14
    assert result["post_adoption"]["FP"] == 13
    assert result["post_adoption"]["PDS"] == 1
    missed = next(row for row in result["rows"] if row["case_id"] == "R00")
    assert missed["model_calls"] == 0
    assert missed["refinement_entrypoint"] == "no_fixed_reports_zero_call_no_op"
    assert missed["vulnerable_alerts"] == 0
