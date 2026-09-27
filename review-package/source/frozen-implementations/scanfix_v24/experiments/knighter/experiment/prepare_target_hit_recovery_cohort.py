#!/usr/bin/env python3
"""Freeze every valid vulnerable-side miss from the complete 39-case screen."""

from __future__ import annotations

import argparse
import hashlib
import json
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
    result_path = screen_root / "SCREEN_RESULT.json"
    result = json.loads(result_path.read_text(encoding="utf-8"))
    rows = list(result.get("rows", []) or [])
    if result.get("requested_cases") != 39 or result.get("completed_records") != 39 or len(rows) != 39:
        raise ValueError("The independent 39-case screen must complete before target-miss freeze")
    selected = [row for row in rows if row["status"] == "vulnerable_target_miss"]
    if not selected:
        raise ValueError("No valid vulnerable-side misses in the completed screen")
    frozen = []
    for row in selected:
        case_id = row["case_id"]
        baseline = screen_root / case_id / "RESULT.json"
        if not baseline.is_file() or sha256(baseline) != row["result_sha256"]:
            raise ValueError(f"Paired starting-checker result drift: {case_id}")
        observed = json.loads(baseline.read_text(encoding="utf-8"))
        if (not observed.get("execution_valid") or observed.get("vulnerable_hit")
                or observed.get("candidate_sha256") != row["checker_sha256"]):
            raise ValueError(f"Target miss no longer matches the frozen checker: {case_id}")
        frozen.append({**row, "starting_feedback_sha256": sha256(baseline)})
    payload = {
        "schema_version": 1,
        "method": "complete_independent_screen_then_all_target_misses",
        "screen_result_sha256": sha256(result_path),
        "screen_manifest_sha256": result["screen_manifest_sha256"],
        "screened_cases": 39,
        "screen_status_counts": dict(Counter(row["status"] for row in rows)),
        "eligible_target_misses": len(frozen),
        "eligible_new_commits_vs_old39": sum(bool(row["new_commit_vs_old39"]) for row in frozen),
        "screened_rows": rows,
        "selected_rows": frozen,
        "selection_note": (
            "Every independently execution-valid vulnerable-side miss is retained. "
            "No model output influenced selection; old upstream scores are not new labels."
        ),
    }
    output_root = args.output_root.resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    manifest = output_root / "COHORT_MANIFEST.json"
    if manifest.exists():
        if json.loads(manifest.read_text(encoding="utf-8")) != payload:
            raise ValueError("Existing target-hit cohort manifest differs")
    else:
        manifest.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps({
        "screened_cases": 39,
        "target_misses": len(frozen),
        "new_commits_vs_old39": payload["eligible_new_commits_vs_old39"],
        "manifest_sha256": sha256(manifest),
    }, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
