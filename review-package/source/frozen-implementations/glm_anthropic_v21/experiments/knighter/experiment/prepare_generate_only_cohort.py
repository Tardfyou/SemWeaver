#!/usr/bin/env python3
"""Freeze all paired-valid, fixed-noisy generate-only cases for matched refinement.

The screen is independent of either refinement method. Every screened case is
retained in the provenance manifest, including misses and execution failures.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import shutil
from collections import Counter
from pathlib import Path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--screen-root", required=True, type=Path)
    parser.add_argument("--output-root", required=True, type=Path)
    args = parser.parse_args()
    screen_root = args.screen_root.resolve()
    output_root = args.output_root.resolve()
    result_path = screen_root / "SCREEN_RESULT.json"
    result = json.loads(result_path.read_text(encoding="utf-8"))
    requested = int(result["requested_cases"])
    rows = list(result["rows"])
    if requested != 39 or result["completed_records"] != requested or len(rows) != requested:
        raise ValueError("The 39-case independent screen must complete before cohort freeze")

    selected = [row for row in rows if row["status"] == "fixed_noisy_refinable"]
    output_root.mkdir(parents=True, exist_ok=True)
    report_root = output_root / "fixed_reports"
    cohort_rows = []
    manifest_rows = []
    for rank, row in enumerate(selected, start=1):
        case_id = row["case_id"]
        source_reports = sorted((screen_root / case_id / "fixed").rglob("report-*.html"))
        if not source_reports:
            raise ValueError(f"No fixed-side report for eligible case {case_id}")
        destination = report_root / case_id / "fixed"
        destination.mkdir(parents=True, exist_ok=True)
        report_hashes = {}
        for index, source in enumerate(source_reports, start=1):
            name = f"report-{index:03d}.html"
            target = destination / name
            source_hash = sha256(source)
            if target.exists():
                if sha256(target) != source_hash:
                    raise ValueError(f"Frozen report drift: {target}")
            else:
                shutil.copyfile(source, target)
            report_hashes[name] = source_hash
        cohort_rows.append({
            "selection_rank": rank,
            "case_id": case_id,
            "commit_id": row["commit_id"],
            "vulnerable_alerts": row["vulnerable_alerts"],
            "fixed_alerts": row["fixed_alerts"],
            "new_commit_vs_old39": row["new_commit_vs_old39"],
        })
        manifest_rows.append({**row, "frozen_fixed_report_sha256": report_hashes})

    cohort_path = output_root / "COHORT.csv"
    fieldnames = (
        "selection_rank", "case_id", "commit_id", "vulnerable_alerts",
        "fixed_alerts", "new_commit_vs_old39",
    )
    if cohort_path.exists():
        with cohort_path.open(newline="", encoding="utf-8") as handle:
            if list(csv.DictReader(handle)) != [
                {key: str(value) for key, value in row.items()} for row in cohort_rows
            ]:
                raise ValueError("Existing cohort CSV differs")
    else:
        with cohort_path.open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=fieldnames)
            writer.writeheader()
            writer.writerows(cohort_rows)

    manifest = {
        "schema_version": 1,
        "method": "complete_independent_screen_then_all_fixed_noisy_eligibles",
        "screen_result_sha256": sha256(result_path),
        "screen_manifest_sha256": result["screen_manifest_sha256"],
        "screened_cases": requested,
        "screen_status_counts": dict(Counter(row["status"] for row in rows)),
        "eligible_cases": len(selected),
        "eligible_new_commits_vs_old39": sum(bool(row["new_commit_vs_old39"]) for row in selected),
        "cohort_csv_sha256": sha256(cohort_path),
        "screened_rows": rows,
        "selected_rows": manifest_rows,
        "selection_note": (
            "Eligibility requires independent vulnerable hit and fixed-side noise. "
            "Old upstream score_valid=False is not treated as a new label. "
            "Neither method output influences this selection."
        ),
    }
    manifest_path = output_root / "COHORT_MANIFEST.json"
    if manifest_path.exists():
        if json.loads(manifest_path.read_text(encoding="utf-8")) != manifest:
            raise ValueError("Existing frozen cohort manifest differs")
    else:
        manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps({
        "screened_cases": requested,
        "status_counts": manifest["screen_status_counts"],
        "eligible_cases": len(selected),
        "eligible_new_commits_vs_old39": manifest["eligible_new_commits_vs_old39"],
        "cohort_sha256": sha256(cohort_path),
    }, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
