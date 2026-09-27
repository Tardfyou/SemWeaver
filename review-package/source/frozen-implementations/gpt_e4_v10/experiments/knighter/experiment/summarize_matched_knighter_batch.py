#!/usr/bin/env python3
"""Independently audit and summarize a matched-Knighter batch."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
from collections import Counter
from pathlib import Path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("batch_root", type=Path)
    parser.add_argument("--output-json", required=True, type=Path)
    parser.add_argument("--output-csv", required=True, type=Path)
    return parser.parse_args()


def classify(result: dict, run_log: str) -> str:
    attempts = result.get("results", [])
    if any(attempt.get("refined") for attempt in attempts):
        return "refined_pds"
    if attempts and attempts[-1].get("result") == "No-FP":
        if attempts[-1].get("num_TP", 0) > 0:
            return "fixed_report_triage_miss"
        return "no_actionable_fixed_report"
    if "Failed to compile refined code" in run_log:
        return "candidate_compile_failure"
    if "Failed to validate on objects" in run_log:
        return "residual_fixed_noise"
    if "Failed to validate on the original commit" in run_log:
        return "validity_loss"
    return "other_failure"


def binomial_cdf(k: int, n: int, probability: float) -> float:
    return sum(
        math.comb(n, i)
        * probability**i
        * (1.0 - probability) ** (n - i)
        for i in range(k + 1)
    )


def bisect_increasing(function, target: float) -> float:
    low, high = 0.0, 1.0
    for _ in range(100):
        middle = (low + high) / 2.0
        if function(middle) < target:
            low = middle
        else:
            high = middle
    return (low + high) / 2.0


def clopper_pearson(successes: int, trials: int, alpha: float = 0.05) -> tuple[float, float]:
    if not 0 <= successes <= trials or trials <= 0:
        raise ValueError("Invalid binomial counts")
    if successes == 0:
        lower = 0.0
    else:
        lower = bisect_increasing(
            lambda probability: 1.0
            - binomial_cdf(successes - 1, trials, probability),
            alpha / 2.0,
        )
    if successes == trials:
        upper = 1.0
    else:
        # CDF is decreasing in p, so invert 1-CDF with an increasing bisection.
        upper = bisect_increasing(
            lambda probability: 1.0
            - binomial_cdf(successes, trials, probability),
            1.0 - alpha / 2.0,
        )
    return lower, upper


def main() -> int:
    args = parse_args()
    batch_root = args.batch_root.resolve()
    manifest_path = batch_root / "BATCH_MANIFEST.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    result_paths = sorted(batch_root.glob("KN-*/MATCHED_BASELINE_RESULT.json"))
    results = {}
    for path in result_paths:
        result = json.loads(path.read_text(encoding="utf-8"))
        case_id = result["case_id"]
        if case_id in results:
            raise ValueError(f"Duplicate result for {case_id}")
        results[case_id] = (path, result)

    rows = []
    for frozen in manifest["cases"]:
        case_id = frozen["case_id"]
        if case_id not in results:
            raise ValueError(f"Missing result for {case_id}")
        path, result = results[case_id]
        if result["checker_sha256"] != frozen["checker_sha256"]:
            raise ValueError(f"Checker hash drift for {case_id}")
        if result["patch_sha256"] != frozen["patch_sha256"]:
            raise ValueError(f"Patch hash drift for {case_id}")
        run_log_path = path.parent / "MATCHED_BASELINE_RUN.log"
        run_log = run_log_path.read_text(encoding="utf-8", errors="ignore")
        category = classify(result, run_log)
        usage = result.get("llm_usage", [])
        attempts = result.get("results", [])
        rows.append(
            {
                "selection_rank": frozen["selection_rank"],
                "case_id": case_id,
                "execution_valid": bool(result.get("execution_valid")),
                "outcome": category,
                "accepted_refinement": category == "refined_pds",
                "fixed_reports_available": result["fixed_report_count_available"],
                "fixed_reports_used": result["fixed_report_count_used"],
                "triage_tp": sum(attempt.get("num_TP", 0) for attempt in attempts),
                "triage_fp": sum(attempt.get("num_FP", 0) for attempt in attempts),
                "llm_calls": len(usage),
                "prompt_tokens": sum(item.get("prompt_tokens", 0) for item in usage),
                "completion_tokens": sum(
                    item.get("completion_tokens", 0) for item in usage
                ),
                "total_tokens": sum(item.get("total_tokens", 0) for item in usage),
                "result_sha256": sha256(path),
                "run_log_sha256": sha256(run_log_path),
            }
        )

    if len(rows) != manifest["case_count"]:
        raise ValueError("Result count does not match frozen manifest")
    if not all(row["execution_valid"] for row in rows):
        invalid = [row["case_id"] for row in rows if not row["execution_valid"]]
        raise ValueError(f"Execution-invalid rows present: {invalid}")

    outcomes = Counter(row["outcome"] for row in rows)
    accepted = sum(row["accepted_refinement"] for row in rows)
    exact_ci = clopper_pearson(accepted, len(rows))
    summary = {
        "schema_version": 1,
        "method": "independent_matched_knighter_batch_audit",
        "batch_manifest_sha256": sha256(manifest_path),
        "model": manifest["model"],
        "wire_api": manifest["wire_api"],
        "reasoning_effort": manifest["reasoning_effort"],
        "subjects": len(rows),
        "execution_valid": sum(row["execution_valid"] for row in rows),
        "accepted_refinements": accepted,
        "accepted_fraction": accepted / len(rows),
        "accepted_exact_95_ci": {"lower": exact_ci[0], "upper": exact_ci[1]},
        "outcomes": dict(sorted(outcomes.items())),
        "llm_calls": sum(row["llm_calls"] for row in rows),
        "prompt_tokens": sum(row["prompt_tokens"] for row in rows),
        "completion_tokens": sum(row["completion_tokens"] for row in rows),
        "total_tokens": sum(row["total_tokens"] for row in rows),
        "rows": rows,
    }
    args.output_json.parent.mkdir(parents=True, exist_ok=True)
    args.output_csv.parent.mkdir(parents=True, exist_ok=True)
    args.output_json.write_text(
        json.dumps(summary, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    with args.output_csv.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    print(json.dumps({key: value for key, value in summary.items() if key != "rows"}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
