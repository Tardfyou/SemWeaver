#!/usr/bin/env python3
"""Reconcile one feedback-loop native/no-internal SemWeaver replicate.

Same per-case caps and feedback workflow are verified, but a single stochastic
decode per arm cannot establish causal benefit from analyzer-internal evidence.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
from pathlib import Path


def load(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def exact_p(a_only: int, b_only: int) -> float:
    n = a_only + b_only
    if not n:
        return 1.0
    return min(1.0, 2 * sum(math.comb(n, i) for i in range(min(a_only, b_only) + 1)) / 2**n)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native-manifest", type=Path, required=True)
    parser.add_argument("--native-result", type=Path, required=True)
    parser.add_argument("--no-internal-manifest", type=Path, required=True)
    parser.add_argument("--no-internal-result", type=Path, required=True)
    parser.add_argument("--screening", type=Path, required=True)
    parser.add_argument("--output-json", type=Path)
    args = parser.parse_args()
    nm = load(args.native_manifest)
    nr = load(args.native_result)
    am = load(args.no_internal_manifest)
    ar = load(args.no_internal_result)
    with args.screening.open(newline="", encoding="utf-8-sig") as handle:
        screening = list(csv.DictReader(handle))
    assert len(screening) == 39
    assert nm["cases"] == am["cases"]
    assert nm["total_model_call_cap"] == am["total_model_call_cap"]
    assert nm["total_model_call_cap"] > 0
    matched_cap = nm["total_model_call_cap"]
    assert nm["implementation_revision"] == am["implementation_revision"]
    assert nm["baseline_summary_sha256"] == am["baseline_summary_sha256"]
    assert sha(args.native_manifest) == nr["batch_manifest_sha256"]
    assert sha(args.no_internal_manifest) == ar["batch_manifest_sha256"]
    assert nr["requested_cases"] == nr["completed_records"] == 12
    assert ar["requested_cases"] == ar["completed_records"] == 12
    nrows = {row["case_id"]: row for row in nr["rows"]}
    arows = {row["case_id"]: row for row in ar["rows"]}
    assert len(nrows) == len(arows) == 12 and nrows.keys() == arows.keys()
    assert all(nrows[key]["call_cap"] == arows[key]["call_cap"] for key in nrows)
    assert nr["llm_calls"] <= matched_cap and ar["llm_calls"] <= matched_cap

    fixed_by_case = {}
    for case_id in nrows:
        prefix = case_id.split("_", 2)[1]
        hits = [row for row in screening if row["commit_id"].startswith(prefix)]
        assert len(hits) == 1
        fixed_by_case[case_id] = int(hits[0]["fixed_alerts"])
    assert sum(fixed_by_case.values()) == 37
    ns = {key for key, row in nrows.items() if row["accepted_pds"] is True}
    ass = {key for key, row in arows.items() if row["accepted_pds"] is True}
    assert len(ns) == nr["accepted_pds"] and len(ass) == ar["accepted_pds"]
    native_only, ablation_only = ns - ass, ass - ns
    result = {
        "status": "completed_single_replicate_feedback_loop_ablation",
        "input_sha256": {
            "native_manifest": sha(args.native_manifest),
            "native_result": sha(args.native_result),
            "no_internal_manifest": sha(args.no_internal_manifest),
            "no_internal_result": sha(args.no_internal_result),
            "screening": sha(args.screening),
            "analysis_script": sha(Path(__file__)),
        },
        "subjects": 12,
        "model_call_cap_per_arm": matched_cap,
        "native_actual_calls": nr["llm_calls"],
        "no_internal_actual_calls": ar["llm_calls"],
        "native_pds": len(ns),
        "no_internal_pds": len(ass),
        "both_success_case_ids": sorted(ns & ass),
        "native_only_case_ids": sorted(native_only),
        "no_internal_only_case_ids": sorted(ablation_only),
        "paired_mcnemar_exact_two_sided_p": exact_p(len(native_only), len(ablation_only)),
        "starting_fixed_alerts": 37,
        "native_retained_fixed_alerts": 37 - sum(fixed_by_case[key] for key in ns),
        "no_internal_retained_fixed_alerts": 37 - sum(fixed_by_case[key] for key in ass),
        "native_outcomes": nr["outcomes"],
        "no_internal_outcomes": ar["outcomes"],
        "native_tokens": nr["total_tokens"],
        "no_internal_tokens": ar["total_tokens"],
        "native_elapsed_seconds": nr["elapsed_seconds"],
        "no_internal_elapsed_seconds": ar["elapsed_seconds"],
        "caution": "One decode per subject on development-informed inputs. A one-run discordance does not establish causal evidence utility or unseen-code robustness."
    }
    rendered = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.output_json:
        args.output_json.parent.mkdir(parents=True, exist_ok=True)
        args.output_json.write_text(rendered, encoding="utf-8")
    print(rendered, end="")


if __name__ == "__main__":
    main()
