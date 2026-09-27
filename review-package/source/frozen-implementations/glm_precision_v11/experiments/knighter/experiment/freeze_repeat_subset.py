#!/usr/bin/env python3
"""Freeze an outcome-blind 12-case E3/E4 repeated-decode subset."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


SALT = "semweaver-fse2027-e3-e4-repeat-v1"


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def rank(case_id: str) -> str:
    return hashlib.sha256(f"{SALT}:{case_id}".encode("utf-8")).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--screen", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    screen_path = args.screen.resolve()
    output = args.output_dir.resolve()
    if output.exists():
        raise FileExistsError(output)
    screen = json.loads(screen_path.read_text(encoding="utf-8"))
    rows = screen.get("rows", [])
    if screen.get("requested_cases") != 39 or screen.get("completed_records") != 39 or len(rows) != 39:
        raise ValueError("Complete 39-case starting screen required")
    precision = [
        row for row in rows if row.get("execution_valid", False)
        and int(row["vulnerable_alerts"]) > 0 and int(row["fixed_alerts"]) > 0
    ]
    recovery = [
        row for row in rows if row.get("execution_valid", False)
        and int(row["vulnerable_alerts"]) == 0
    ]
    if len(precision) != 14 or len(recovery) != 25:
        raise ValueError("Starting-status strata drift")
    selected = {
        "precision": sorted(precision, key=lambda row: rank(row["case_id"]))[:4],
        "recovery": sorted(recovery, key=lambda row: rank(row["case_id"]))[:8],
    }
    output.mkdir(parents=True)
    for name, rows_for_stratum in selected.items():
        cohort = {
            "schema_version": 1,
            "method": (
                "complete_independent_screen_then_all_fixed_noisy_eligibles"
                if name == "precision"
                else "complete_independent_screen_then_all_target_misses"
            ),
            "screen_result_sha256": sha256(screen_path),
            "selection_type": "repeated_decode_subset_not_full_cohort",
            "source_stratum_count": 14 if name == "precision" else 25,
            "selection_rule": "lowest SHA-256 of fixed salt plus pre-treatment case ID within stratum",
            "selected_rows": [
                {"case_id": row["case_id"], "starting_status": row.get("status", ""),
                 "selection_hash": rank(row["case_id"])}
                for row in rows_for_stratum
            ],
        }
        (output / f"{name.upper()}_COHORT_MANIFEST.json").write_text(
            json.dumps(cohort, indent=2) + "\n", encoding="utf-8"
        )
    summary = {
        "schema_version": 1,
        "method": "outcome_blind_stratified_sha256_repeat_subset",
        "salt": SALT,
        "screen_result_sha256": sha256(screen_path),
        "precision_count": 4,
        "recovery_count": 8,
        "case_ids": {
            name: [row["case_id"] for row in rows_for_stratum]
            for name, rows_for_stratum in selected.items()
        },
        "inference_boundary": (
            "Repeated decodes on the same 12 preselected patches estimate model "
            "variability, not 12 new independent subjects or full-39 variance."
        ),
    }
    (output / "SUBSET_MANIFEST.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(summary["case_ids"], indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
