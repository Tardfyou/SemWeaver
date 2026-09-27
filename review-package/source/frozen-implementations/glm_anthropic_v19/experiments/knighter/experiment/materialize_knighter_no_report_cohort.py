#!/usr/bin/env python3
"""Freeze the 25 zero-report target misses for KNighter's no-op branch."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--screen", required=True, type=Path)
    parser.add_argument("--recovery-cohort", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    screen_path = args.screen.resolve()
    cohort_path = args.recovery_cohort.resolve()
    output = args.output_dir.resolve()
    if output.exists():
        raise FileExistsError(output)
    screen = json.loads(screen_path.read_text(encoding="utf-8"))
    cohort = json.loads(cohort_path.read_text(encoding="utf-8"))
    if screen.get("requested_cases") != 39 or screen.get("completed_records") != 39:
        raise ValueError("Complete 39-case screen required")
    if cohort.get("method") != "complete_independent_screen_then_all_target_misses":
        raise ValueError("Recovery cohort is not the complete target-miss stratum")
    by_case = {row["case_id"]: row for row in screen["rows"]}
    selected = [row["case_id"] for row in cohort["selected_rows"]]
    if len(by_case) != 39 or len(selected) != 25 or len(set(selected)) != 25:
        raise ValueError("Unexpected screened or recovery cohort denominator")
    if cohort.get("screen_result_sha256") != sha256(screen_path):
        raise ValueError("Recovery cohort screen hash drift")
    for case_id in selected:
        row = by_case.get(case_id)
        if (
            not row or not row.get("execution_valid", False)
            or int(row.get("vulnerable_alerts", -1)) != 0
            or int(row.get("fixed_alerts", -1)) != 0
        ):
            raise ValueError(f"Not a valid zero-report target miss: {case_id}")
    output.mkdir(parents=True)
    csv_path = output / "COHORT.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=[
            "selection_rank", "case_id", "commit_id", "vulnerable_alerts",
            "fixed_alerts", "new_commit_vs_old39",
        ])
        writer.writeheader()
        for index, case_id in enumerate(selected, 1):
            row = by_case[case_id]
            writer.writerow({
                "selection_rank": index,
                "case_id": case_id,
                "commit_id": row["commit_id"],
                "vulnerable_alerts": row["vulnerable_alerts"],
                "fixed_alerts": row["fixed_alerts"],
                "new_commit_vs_old39": row.get("new_commit_vs_old39", False),
            })
            (output / "fixed_reports" / case_id / "fixed").mkdir(parents=True)
    manifest = {
        "schema_version": 1,
        "method": "complete_frozen_target_misses_for_knighter_zero_report_branch",
        "screen_result_sha256": sha256(screen_path),
        "recovery_cohort_manifest_sha256": sha256(cohort_path),
        "cohort_csv_sha256": sha256(csv_path),
        "cases": selected,
        "count": len(selected),
        "report_count_each": 0,
        "intervention": "automatic no-report refinement no-op",
    }
    (output / "COHORT_MANIFEST.json").write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    print(json.dumps({"count": len(selected), "cohort_csv_sha256": manifest["cohort_csv_sha256"]}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
