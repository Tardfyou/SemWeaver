#!/usr/bin/env python3
"""Bind the actual KNighter refinement loop to all 39 generated checkers.

KNighter's report-driven refiner runs on all cases with fixed-side reports.
The no-report branch is an explicit zero-call no-op, not an attempted and
failed target-hit recovery. Every checker still receives the common paired
starting evaluation, so the 39-case system output has a complete denominator.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def score(rows: list[dict]) -> dict:
    tp = sum(row["vulnerable_alerts"] > 0 for row in rows)
    fp = sum(row["fixed_alerts"] > 0 for row in rows)
    fn = len(rows) - tp
    tn = len(rows) - fp
    denominator = 2 * tp + fp + fn
    return {
        "patch_pairs": len(rows), "patch_version_decisions": 2 * len(rows),
        "TP": tp, "FP": fp, "FN": fn, "TN": tn,
        "PDS": sum(row["vulnerable_alerts"] > 0 and row["fixed_alerts"] == 0 for row in rows),
        "fixed_object_alerts": sum(row["fixed_alerts"] for row in rows),
        "precision": tp / (tp + fp) if tp + fp else None,
        "recall": tp / (tp + fn) if tp + fn else None,
        "F1": 2 * tp / denominator if denominator else None,
    }


def summarize(screen_path: Path, baseline_dir: Path, strict_dir: Path,
              no_report_dir: Path) -> dict:
    screen = json.loads(screen_path.read_text(encoding="utf-8"))
    if screen.get("requested_cases") != 39 or screen.get("completed_records") != 39:
        raise ValueError("Complete 39-case paired screen required")
    starting = {row["case_id"]: row for row in screen["rows"]}
    if len(starting) != 39 or any(not row.get("execution_valid", False) for row in starting.values()):
        raise ValueError("Starting checker identities or paired execution invalid")
    batch_path = baseline_dir / "BATCH_RESULT.json"
    strict_path = strict_dir / "STRICT_RESULT.json"
    batch = json.loads(batch_path.read_text(encoding="utf-8"))
    strict = json.loads(strict_path.read_text(encoding="utf-8"))
    strict_manifest_path = strict_dir / "STRICT_MANIFEST.json"
    strict_manifest = json.loads(strict_manifest_path.read_text(encoding="utf-8"))
    if (
        batch.get("requested_cases") != 14 or batch.get("execution_valid") != 14
        or strict.get("subjects") != 14 or strict.get("unscored") != 0
        or strict.get("strict_manifest_sha256") != sha256(strict_manifest_path)
        or strict_manifest.get("baseline_batch_result_sha256") != sha256(batch_path)
    ):
        raise ValueError("KNighter 14-case report-driven baseline incomplete")
    baseline_rows = {item["case_id"]: item["result"] for item in batch["cases"]}
    strict_rows = {item["case_id"]: item for item in strict["rows"]}
    if len(baseline_rows) != 14 or set(strict_rows) != set(baseline_rows):
        raise ValueError("KNighter strict validation identities differ from baseline")
    if any(not result.get("execution_valid", False) for result in baseline_rows.values()):
        raise ValueError("KNighter report-driven baseline has invalid execution")
    expected_report_cases = {
        case_id for case_id, row in starting.items()
        if int(row["vulnerable_alerts"]) > 0 and int(row["fixed_alerts"]) > 0
    }
    if set(baseline_rows) != expected_report_cases:
        raise ValueError("KNighter report-driven cases do not match all 14 fixed-noisy subjects")
    no_report_path = no_report_dir / "BATCH_RESULT.json"
    no_report_batch = json.loads(no_report_path.read_text(encoding="utf-8"))
    no_report_rows = {item["case_id"]: item["result"] for item in no_report_batch["cases"]}
    expected_no_report = set(starting) - expected_report_cases
    if (
        no_report_batch.get("requested_cases") != 25
        or no_report_batch.get("execution_valid") != 25
        or len(no_report_rows) != 25
        or set(no_report_rows) != expected_no_report
    ):
        raise ValueError("Complete 25-case KNighter zero-report branch required")
    for case_id, no_report in no_report_rows.items():
        if (
            not no_report.get("execution_valid", False)
            or not no_report.get("no_fixed_reports", False)
            or int(no_report.get("fixed_report_count_used", -1)) != 0
            or int(no_report.get("model_calls_used", -1)) != 0
            or no_report.get("checker_sha256") != starting[case_id]["checker_sha256"]
            or any(attempt.get("refined", False) for attempt in no_report.get("results", []))
            or not any(attempt.get("result") == "Perfect" for attempt in no_report.get("results", []))
        ):
            raise ValueError(f"Zero-report KNighter run is not a verified no-op: {case_id}")
    rows = []
    for case_id, start in starting.items():
        vulnerable = int(start["vulnerable_alerts"])
        fixed = int(start["fixed_alerts"])
        report_driven = case_id in baseline_rows
        adopted = False
        calls = 0
        candidate_sha = start["checker_sha256"]
        if report_driven:
            baseline = baseline_rows[case_id]
            strict_row = strict_rows[case_id]
            calls = int(baseline.get("model_calls_used", -1))
            if calls < 0 or calls != int(strict_row.get("llm_calls", -2)):
                raise ValueError(f"Model-call ledger mismatch: {case_id}")
            if strict_row.get("strict_pds") is True:
                validation_path = strict_dir / case_id / "RESULT.json"
                if sha256(validation_path) != strict_row.get("result_sha256"):
                    raise ValueError(f"Strict paired validation hash mismatch: {case_id}")
                validation = json.loads(validation_path.read_text(encoding="utf-8"))
                if not validation.get("execution_valid", False) or not validation.get("pds", False):
                    raise ValueError(f"Claimed strict PDS lacks independent replay: {case_id}")
                vulnerable = int(validation["vulnerable_alerts"])
                fixed = int(validation["fixed_alerts"])
                candidate_sha = validation["candidate_sha256"]
                adopted = True
        elif vulnerable != 0 or fixed != 0:
            raise ValueError(f"No-report branch is not a target-miss case: {case_id}")
        rows.append({
            "case_id": case_id,
            "starting_vulnerable_alerts": int(start["vulnerable_alerts"]),
            "starting_fixed_alerts": int(start["fixed_alerts"]),
            "vulnerable_alerts": vulnerable,
            "fixed_alerts": fixed,
            "candidate_sha256": candidate_sha,
            "refinement_entrypoint": (
                "actual_knighter_report_loop" if report_driven
                else "no_fixed_reports_zero_call_no_op"
            ),
            "refinement_applicable": report_driven,
            "model_calls": calls,
            "adopted_strict_pds": adopted,
        })
    baseline_counts = score([
        {"vulnerable_alerts": row["starting_vulnerable_alerts"],
         "fixed_alerts": row["starting_fixed_alerts"]}
        for row in rows
    ])
    output = {
        "schema_version": 1,
        "method": "knighter_generated_checker_plus_actual_report_refinement_complete_39",
        "screen_result_sha256": sha256(screen_path),
        "baseline_batch_result_sha256": sha256(batch_path),
        "no_report_batch_result_sha256": sha256(no_report_path),
        "strict_baseline_result_sha256": sha256(strict_path),
        "subjects": 39,
        "report_refinement_cases": 14,
        "no_report_no_op_cases": 25,
        "baseline": baseline_counts,
        "post_adoption": score(rows),
        "strata": {
            name: score([row for row in rows if row["refinement_applicable"] == applicable])
            for name, applicable in (("fixed_noisy", True), ("target_miss", False))
        },
        "rows": rows,
        "interpretation": (
            "All 39 start from automatic KNighter generation. The report-driven "
            "refinement loop executes on the 14 with fixed reports; the 25 "
            "zero-report target misses are explicit no-ops with zero model calls. "
            "This is a full-system comparison, not a claim that KNighter's "
            "false-positive subroutine attempted target-hit recovery."
        ),
    }
    return output


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--screen", required=True, type=Path)
    parser.add_argument("--baseline-dir", required=True, type=Path)
    parser.add_argument("--strict-dir", required=True, type=Path)
    parser.add_argument("--no-report-dir", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    payload = summarize(
        args.screen.resolve(), args.baseline_dir.resolve(),
        args.strict_dir.resolve(), args.no_report_dir.resolve(),
    )
    output_path = args.output.resolve()
    if output_path.exists():
        raise FileExistsError(output_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(payload, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps({key: payload[key] for key in (
        "subjects", "report_refinement_cases", "no_report_no_op_cases",
        "baseline", "post_adoption",
    )}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
