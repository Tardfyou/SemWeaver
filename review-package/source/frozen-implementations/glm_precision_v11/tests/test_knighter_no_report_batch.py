import csv
import importlib.util
import json
import sys
from pathlib import Path

import pytest


SCRIPT = (
    Path(__file__).resolve().parents[1]
    / "experiments" / "knighter" / "experiment"
    / "run_matched_knighter_batch.py"
)
sys.path.insert(0, str(SCRIPT.parent))
SPEC = importlib.util.spec_from_file_location("knighter_no_report_batch", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC and SPEC.loader
SPEC.loader.exec_module(MODULE)


def test_zero_report_branch_requires_bound_starting_miss(tmp_path: Path, monkeypatch):
    case_id = "case_zero"
    cases = tmp_path / "cases"
    case = cases / case_id
    (case / "metadata").mkdir(parents=True)
    (case / "csa").mkdir()
    (case / "patches").mkdir()
    (case / "metadata" / "candidate.json").write_text("{}\n")
    checker = case / "csa" / "SAGenTestChecker.cpp"
    checker.write_text("checker\n")
    (case / "patches" / "knighter_patch.md").write_text("patch\n")
    reports = tmp_path / "reports"
    (reports / case_id / "fixed").mkdir(parents=True)
    cohort = tmp_path / "COHORT.csv"
    with cohort.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=["selection_rank", "case_id", "commit_id"])
        writer.writeheader()
        writer.writerow({"selection_rank": 1, "case_id": case_id, "commit_id": "commit"})
    screen = tmp_path / "SCREEN_RESULT.json"
    screen.write_text(json.dumps({
        "requested_cases": 39, "completed_records": 39,
        "rows": [
            {"case_id": case_id, "execution_valid": True, "vulnerable_alerts": 0,
             "fixed_alerts": 0, "checker_sha256": MODULE.sha256(checker),
             "commit_id": "commit", "result_sha256": "paired-hash"},
            *({"case_id": f"other-{i}"} for i in range(38)),
        ],
    }) + "\n")
    argv = [
        "batch", "--cohort-csv", str(cohort), "--cases-root", str(cases),
        "--fixed-reports-root", str(reports), "--linux-dir", str(tmp_path),
        "--output-root", str(tmp_path / "output"),
        "--backend-root", str(tmp_path / "backend"), "--freeze-only",
    ]
    monkeypatch.setattr(sys, "argv", argv)
    with pytest.raises(RuntimeError, match="Incomplete frozen input"):
        MODULE.main()
    monkeypatch.setattr(sys, "argv", argv + [
        "--allow-empty-fixed-reports", "--starting-screen-result", str(screen),
    ])
    assert MODULE.main() == 0
    manifest = json.loads((tmp_path / "output" / "BATCH_MANIFEST.json").read_text())
    assert manifest["cases"][0]["no_fixed_reports"] is True
    assert manifest["cases"][0]["starting_screen_result_sha256"] == "paired-hash"
