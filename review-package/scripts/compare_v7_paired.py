#!/usr/bin/env python3
"""Reconcile a feedback-loop SemWeaver batch with its strict KNighter baseline."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
from pathlib import Path


def read(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def exact_mcnemar_p(a_only: int, b_only: int) -> float:
    n = a_only + b_only
    if not n:
        return 1.0
    return min(1.0, 2 * sum(math.comb(n, i) for i in range(min(a_only, b_only) + 1)) / 2**n)


def version_f1(fixed_positive_decisions: int) -> float:
    # 39 positive patch versions remain hits under PDS-only adoption.
    return 78 / (78 + fixed_positive_decisions)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-strict", required=True, type=Path)
    parser.add_argument("--treatment-manifest", required=True, type=Path)
    parser.add_argument("--treatment-result", required=True, type=Path)
    parser.add_argument("--screening", required=True, type=Path)
    parser.add_argument("--output-json", type=Path)
    args = parser.parse_args()
    base = read(args.baseline_strict)
    manifest = read(args.treatment_manifest)
    treatment = read(args.treatment_result)
    with args.screening.open(newline="", encoding="utf-8-sig") as handle:
        screen = list(csv.DictReader(handle))

    assert len(screen) == 39
    assert base["subjects"] == treatment["requested_cases"] == treatment["completed_records"] == 12
    assert base["unscored"] == 0
    assert sha(args.treatment_manifest) == treatment["batch_manifest_sha256"]
    brows = {row["case_id"]: row for row in base["rows"]}
    trows = {row["case_id"]: row for row in treatment["rows"]}
    mrows = {row["case_id"]: row for row in manifest["cases"]}
    assert len(brows) == len(trows) == len(mrows) == 12
    assert brows.keys() == trows.keys() == mrows.keys()
    assert all(brows[key]["llm_calls"] == trows[key]["call_cap"] == mrows[key]["call_cap"] for key in brows)
    assert sum(row["call_cap"] for row in trows.values()) == base["baseline_model_calls"] == manifest["total_model_call_cap"]
    assert treatment["llm_calls"] <= manifest["total_model_call_cap"]

    fixed_by_case = {}
    for case_id in brows:
        prefix = case_id.split("_", 2)[1]
        hits = [row for row in screen if row["commit_id"].startswith(prefix)]
        assert len(hits) == 1
        assert int(hits[0]["buggy_alerts"]) > 0 and int(hits[0]["fixed_alerts"]) > 0
        fixed_by_case[case_id] = int(hits[0]["fixed_alerts"])
    assert sum(fixed_by_case.values()) == 37
    bsuccess = {key for key, row in brows.items() if row["strict_pds"] is True}
    tsuccess = {key for key, row in trows.items() if row["accepted_pds"] is True}
    assert len(bsuccess) == base["strict_pds"]
    assert len(tsuccess) == treatment["accepted_pds"]
    b_only, t_only = bsuccess - tsuccess, tsuccess - bsuccess
    b_fixed = 37 - sum(fixed_by_case[key] for key in bsuccess)
    t_fixed = 37 - sum(fixed_by_case[key] for key in tsuccess)
    result = {
        "status": "completed_single_replicate_feedback_loop_comparison",
        "input_sha256": {
            "baseline_strict": sha(args.baseline_strict),
            "treatment_manifest": sha(args.treatment_manifest),
            "treatment_result": sha(args.treatment_result),
            "screening": sha(args.screening),
            "analysis_script": sha(Path(__file__)),
        },
        "subjects": 12,
        "matched_call_cap": manifest["total_model_call_cap"],
        "baseline_actual_calls": base["baseline_model_calls"],
        "treatment_actual_calls": treatment["llm_calls"],
        "baseline_pds": len(bsuccess),
        "treatment_pds": len(tsuccess),
        "both_success_case_ids": sorted(bsuccess & tsuccess),
        "baseline_only_case_ids": sorted(b_only),
        "treatment_only_case_ids": sorted(t_only),
        "paired_mcnemar_exact_two_sided_p": exact_mcnemar_p(len(b_only), len(t_only)),
        "starting_fixed_alerts": 37,
        "baseline_retained_fixed_alerts": b_fixed,
        "treatment_retained_fixed_alerts": t_fixed,
        "portfolio_39": {
            "unmodified_pds": 27,
            "baseline_adopted_pds": 27 + len(bsuccess),
            "treatment_adopted_pds": 27 + len(tsuccess),
            "unmodified_version_f1": version_f1(12),
            "baseline_adopted_version_f1": version_f1(12 - len(bsuccess)),
            "treatment_adopted_version_f1": version_f1(12 - len(tsuccess)),
        },
        "treatment_outcomes": treatment["outcomes"],
        "treatment_total_tokens": treatment["total_tokens"],
        "treatment_elapsed_seconds": treatment["elapsed_seconds"],
        "treatment_paired_validation_seconds": treatment["paired_validation_seconds"],
        "caution": "A single decode on development-informed subjects does not estimate model-run variance or unseen-code generalization. Candidate robustness and the no-internal arm are separate gates."
    }
    rendered = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.output_json:
        args.output_json.parent.mkdir(parents=True, exist_ok=True)
        args.output_json.write_text(rendered, encoding="utf-8")
    print(rendered, end="")


if __name__ == "__main__":
    main()
