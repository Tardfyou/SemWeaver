#!/usr/bin/env python3
"""Recompute the historical 39-checker patch-version portfolio metrics.

This is an audit of the original follow-up, not a new SemWeaver treatment or
a head-to-head comparison with KNighter's refinement loop. The corrected
memory-leak baseline comes from the sanitizer revalidation row in the final
results file. Output is JSON on stdout; source files are never modified.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from collections import Counter
from pathlib import Path


def rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8-sig") as handle:
        return list(csv.DictReader(handle))


def metrics(outcomes: list[tuple[int, int]]) -> dict[str, float | int]:
    tp = sum(v > 0 for v, _ in outcomes)
    fn = sum(v == 0 for v, _ in outcomes)
    fp = sum(f > 0 for _, f in outcomes)
    tn = sum(f == 0 for _, f in outcomes)
    return {
        "tp": tp,
        "fp": fp,
        "fn": fn,
        "tn": tn,
        "pds": sum(v > 0 and f == 0 for v, f in outcomes),
        "fixed_alerts": sum(f for _, f in outcomes),
        "precision": tp / (tp + fp) if tp + fp else 0.0,
        "recall": tp / (tp + fn) if tp + fn else 0.0,
        "f1": 2 * tp / (2 * tp + fp + fn) if 2 * tp + fp + fn else 0.0,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--screening", required=True, type=Path)
    parser.add_argument("--final", required=True, type=Path)
    parser.add_argument("--output-json", type=Path)
    args = parser.parse_args()

    screening = rows(args.screening)
    final = rows(args.final)
    assert len(screening) == 39
    assert len({row["commit_id"] for row in screening}) == 39
    complete = {row["commit_id"]: row for row in final if row["status"] == "complete"}
    assert len(complete) == 12
    corrected = [row for row in final if row["status"] == "excluded_from_paper_analysis"]
    assert len(corrected) == 1
    assert corrected[0]["commit_id"].startswith("27834971")
    assert (corrected[0]["baseline_buggy_alerts"], corrected[0]["baseline_fixed_alerts"]) == ("1", "0")

    base: dict[str, tuple[int, int]] = {}
    for row in screening:
        base[row["commit_id"]] = (int(row["buggy_alerts"]), int(row["fixed_alerts"]))
    base[corrected[0]["commit_id"]] = (1, 0)

    statuses = Counter(row["refine_success"] for row in complete.values())
    assert statuses == {"TRUE": 5, "manual_validated": 6, "FALSE": 1}
    automatic = {
        commit: row
        for commit, row in complete.items()
        if row["refine_success"] == "TRUE" and row["refined_pds_strict"] == "TRUE"
    }
    assert len(automatic) == 5
    assert all(base[commit] == (int(row["baseline_buggy_alerts"]), int(row["baseline_fixed_alerts"]))
               for commit, row in complete.items())

    adopted = dict(base)
    for commit, row in automatic.items():
        adopted[commit] = (int(row["refined_buggy_alerts"]), int(row["refined_fixed_alerts"]))

    output = {
        "screening_sha256": hashlib.sha256(args.screening.read_bytes()).hexdigest(),
        "final_sha256": hashlib.sha256(args.final.read_bytes()).hexdigest(),
        "analysis_script_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "scope": "one binary target-hit decision per checker and patch revision",
        "denominators": {"checkers": 39, "patch_version_decisions": 78, "fixed_noisy": 12},
        "intervention": "historical automatic PDS-only adoption; manual completions excluded",
        "historical_statuses": dict(statuses),
        "automatic_case_ids": sorted(row["case_id"] for row in automatic.values()),
        "baseline": metrics(list(base.values())),
        "automatic_adopt_only": metrics(list(adopted.values())),
    }
    assert output["baseline"]["fixed_alerts"] == 37
    assert output["automatic_adopt_only"]["fixed_alerts"] == 23
    rendered = json.dumps(output, indent=2, sort_keys=True) + "\n"
    if args.output_json:
        args.output_json.parent.mkdir(parents=True, exist_ok=True)
        args.output_json.write_text(rendered, encoding="utf-8")
    print(rendered, end="")


if __name__ == "__main__":
    main()
