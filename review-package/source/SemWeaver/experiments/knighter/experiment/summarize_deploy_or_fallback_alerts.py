#!/usr/bin/env python3
"""Summarize fixed-side alerts under PDS-only adoption with baseline fallback."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def load(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cases-root", required=True, type=Path)
    parser.add_argument("--cohort-manifest", required=True, type=Path)
    parser.add_argument("--knighter-summary", required=True, type=Path)
    parser.add_argument("--semweaver-matched", required=True, type=Path)
    parser.add_argument("--semweaver-full", required=True, type=Path)
    return parser.parse_args()


def condition(name: str, accepted: list[str], counts: dict[str, int], calls: int) -> dict:
    removed = sum(counts[case_id] for case_id in accepted)
    baseline = sum(counts.values())
    return {
        "condition": name,
        "model_calls": calls,
        "accepted_subjects": len(accepted),
        "accepted_case_ids": accepted,
        "fixed_alerts_removed": removed,
        "fixed_alerts_retained": baseline - removed,
        "reduction_fraction": removed / baseline,
    }


def main() -> int:
    args = parse_args()
    cohort = load(args.cohort_manifest)
    case_ids = [str(item["case_id"]) for item in cohort["cases"]]
    counts = {
        case_id: int(load(args.cases_root / case_id / "fixed_validation.json")["diagnostics_count"])
        for case_id in case_ids
    }
    knighter = load(args.knighter_summary)
    matched = load(args.semweaver_matched)
    full = load(args.semweaver_full)

    rows = [
        condition(
            "knighter_model_matched",
            [str(knighter["accepted_candidate"]["case_id"])],
            counts,
            int(knighter["usage"]["llm_calls"]),
        ),
        condition(
            "semweaver_call_matched",
            [str(matched["case_id"])],
            counts,
            int(matched["model_calls"]),
        ),
        condition(
            "semweaver_full_budget",
            [str(case_id) for case_id in full["pds"]["case_ids"]],
            counts,
            int(full["llm_usage"]["calls"]),
        ),
    ]
    payload = {
        "schema_version": 1,
        "policy": "Adopt a candidate only when it achieves PDS; otherwise retain the original checker.",
        "subjects": len(case_ids),
        "starting_fixed_alerts": sum(counts.values()),
        "starting_fixed_alerts_by_case": counts,
        "conditions": rows,
        "comparison_limit": "The full-budget SemWeaver condition uses 86 calls versus 19 in each matched condition; its alert reduction is a budget-sensitivity result, not a matched method effect.",
    }
    print(json.dumps(payload, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
