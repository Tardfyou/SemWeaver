from __future__ import annotations

import csv
import json
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "experiments" / "knighter" / "experiment"))
from prepare_generate_only_cohort import main  # noqa: E402
from collect_frozen_csa_evidence_batch import cohort_case_ids  # noqa: E402
import collect_frozen_csa_evidence_batch as collector  # noqa: E402
from prepare_target_hit_recovery_cohort import main as target_miss_main  # noqa: E402


def _screen(root: Path, *, completed: int) -> None:
    rows = []
    for index in range(completed):
        case_id = f"G{index + 1:02d}"
        status = "fixed_noisy_refinable" if index in (1, 17) else "vulnerable_target_miss"
        rows.append({
            "case_id": case_id,
            "commit_id": f"{index:040x}",
            "status": status,
            "vulnerable_alerts": 1 if status == "fixed_noisy_refinable" else 0,
            "fixed_alerts": 1 if status == "fixed_noisy_refinable" else 0,
            "new_commit_vs_old39": index == 17,
        })
        if status == "fixed_noisy_refinable":
            report = root / case_id / "fixed" / "target" / "run" / "report-xyz.html"
            report.parent.mkdir(parents=True)
            report.write_text(f"<title>{case_id}</title>", encoding="utf-8")
    (root / "SCREEN_RESULT.json").write_text(json.dumps({
        "requested_cases": 39,
        "completed_records": completed,
        "screen_manifest_sha256": "a" * 64,
        "rows": rows,
    }), encoding="utf-8")


def test_generate_only_cohort_requires_complete_screen(tmp_path: Path, monkeypatch):
    screen = tmp_path / "screen"
    screen.mkdir()
    _screen(screen, completed=38)
    monkeypatch.setattr(sys, "argv", ["program", "--screen-root", str(screen), "--output-root", str(tmp_path / "cohort")])

    with pytest.raises(ValueError, match="must complete"):
        main()


def test_generate_only_cohort_keeps_all_independently_eligible_cases(tmp_path: Path, monkeypatch):
    screen = tmp_path / "screen"
    screen.mkdir()
    _screen(screen, completed=39)
    output = tmp_path / "cohort"
    monkeypatch.setattr(sys, "argv", ["program", "--screen-root", str(screen), "--output-root", str(output)])

    assert main() == 0
    assert main() == 0
    with (output / "COHORT.csv").open(newline="", encoding="utf-8") as handle:
        cohort = list(csv.DictReader(handle))
    manifest = json.loads((output / "COHORT_MANIFEST.json").read_text(encoding="utf-8"))
    assert [row["case_id"] for row in cohort] == ["G02", "G18"]
    assert manifest["screened_cases"] == 39
    assert manifest["eligible_new_commits_vs_old39"] == 1
    assert len(manifest["screened_rows"]) == 39
    assert cohort_case_ids(manifest) == ["G02", "G18"]


def test_target_hit_cohort_keeps_every_valid_miss_and_binds_source(tmp_path: Path, monkeypatch):
    screen = tmp_path / "screen"
    screen.mkdir()
    rows = []
    for index in range(39):
        case_id = f"G{index + 1:02d}"
        row = {
            "case_id": case_id,
            "status": "vulnerable_target_miss" if index in (2, 20) else "already_pds",
            "checker_sha256": f"{index:064x}",
            "new_commit_vs_old39": index == 20,
        }
        if row["status"] == "vulnerable_target_miss":
            path = screen / case_id / "RESULT.json"
            path.parent.mkdir(parents=True)
            path.write_text(json.dumps({
                "execution_valid": True, "vulnerable_hit": False,
                "candidate_sha256": row["checker_sha256"],
            }), encoding="utf-8")
            from prepare_target_hit_recovery_cohort import sha256
            row["result_sha256"] = sha256(path)
        rows.append(row)
    (screen / "SCREEN_RESULT.json").write_text(json.dumps({
        "requested_cases": 39, "completed_records": 39,
        "screen_manifest_sha256": "a" * 64, "rows": rows,
    }), encoding="utf-8")
    output = tmp_path / "target_cohort"
    monkeypatch.setattr(sys, "argv", [
        "program", "--screen-root", str(screen), "--output-root", str(output)
    ])

    assert target_miss_main() == 0
    manifest = json.loads((output / "COHORT_MANIFEST.json").read_text(encoding="utf-8"))
    assert [row["case_id"] for row in manifest["selected_rows"]] == ["G03", "G21"]
    assert manifest["eligible_new_commits_vs_old39"] == 1
    assert cohort_case_ids(manifest) == ["G03", "G21"]


def test_evidence_batch_progress_never_overwrites_case_manifest(tmp_path: Path, monkeypatch):
    case_id = "case_a"
    cases = tmp_path / "cases"
    case = cases / case_id
    for relative in (
        "patchweaver_plan.json", "patches/commit.patch",
        "csa/SAGenTestChecker.cpp", "metadata/candidate.json",
    ):
        path = case / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(relative + "\n", encoding="utf-8")
    cohort = tmp_path / "cohort.json"
    cohort.write_text(json.dumps({"cases": [{"case_id": case_id}]}), encoding="utf-8")
    config = tmp_path / "config.yaml"
    config.write_text("llm: {}\n", encoding="utf-8")
    linux = tmp_path / "linux"
    linux.mkdir()
    output = tmp_path / "evidence"
    monkeypatch.setattr(sys, "argv", [
        "program", "--config", str(config), "--cases-root", str(cases),
        "--cohort-manifest", str(cohort), "--linux-dir", str(linux),
        "--output-dir", str(output),
    ])

    driver_calls = []

    def fake_run(command, **_kwargs):
        if command[:3] == ["git", "rev-parse", "HEAD"]:
            return SimpleNamespace(stdout="revision\n", stderr="", returncode=0)
        driver_calls.append(command)
        case_output = Path(command[command.index("--output-dir") + 1])
        case_output.mkdir(parents=True)
        (case_output / "EVIDENCE_REPLAY_MANIFEST.json").write_text(json.dumps({
            "method": "frozen_plan_csa_evidence_replay", "eligible": True,
            "decision": "treatment_eligible", "records": 1,
            "origin_counts": {"analyzer_internal": 1},
        }) + "\n", encoding="utf-8")
        return SimpleNamespace(stdout="case stdout", stderr="", returncode=0)

    monkeypatch.setattr(collector.subprocess, "run", fake_run)
    assert collector.main() == 0
    case_manifest = json.loads((output / case_id / "EVIDENCE_REPLAY_MANIFEST.json").read_text())
    batch_result = json.loads((output / "BATCH_RESULT.json").read_text())
    assert case_manifest["method"] == "frozen_plan_csa_evidence_replay"
    assert batch_result["completed_records"] == 1
    assert batch_result["rows"][0]["result_sha256"] == collector.sha256(
        output / case_id / "EVIDENCE_REPLAY_MANIFEST.json"
    )
    monkeypatch.setattr(sys, "argv", sys.argv + ["--resume"])
    assert collector.main() == 0
    assert len(driver_calls) == 1
