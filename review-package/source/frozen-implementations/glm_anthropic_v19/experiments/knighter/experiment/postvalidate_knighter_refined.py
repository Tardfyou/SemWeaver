#!/usr/bin/env python3
"""Apply the strict paired CSA oracle to every upstream-refined candidate."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path


HERE = Path(__file__).resolve().parent
VALIDATOR = HERE / "validate_frozen_csa_candidate.py"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_json(path: Path, value: dict) -> None:
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-batch", required=True, type=Path)
    parser.add_argument("--cases-root", required=True, type=Path)
    parser.add_argument("--linux-dir", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--backend-root", required=True, type=Path)
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--review-policy", choices=("diagnostic", "gate"), default="diagnostic")
    parser.add_argument("--freeze-only", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    baseline_root = args.baseline_batch.resolve()
    batch_result_path = baseline_root / "BATCH_RESULT.json"
    batch = json.loads(batch_result_path.read_text(encoding="utf-8"))
    output_dir = args.output_dir.resolve()
    backend_root = args.backend_root.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    backend_root.mkdir(parents=True, exist_ok=True)

    cases = []
    models = set()
    wire_apis = set()
    reasoning_efforts = set()
    for item in batch["cases"]:
        case_id = str(item["case_id"])
        result = item.get("result") or {}
        if result:
            models.add(str(result.get("model", "")))
            wire_apis.add(str(result.get("wire_api", "")))
            reasoning_efforts.add(str(result.get("reasoning_effort", "")))
        refined = bool(result.get("execution_valid")) and any(
            attempt.get("refined", False) for attempt in result.get("results", [])
        )
        candidate = baseline_root / str(result.get("checker_id", "")) / "refinements" / "latest_refined.cpp"
        candidate_sha = sha256(candidate) if refined and candidate.is_file() else ""
        cases.append(
            {
                "case_id": case_id,
                "baseline_execution_valid": bool(result.get("execution_valid")),
                "upstream_refined": refined,
                "llm_calls": len(result.get("llm_usage", [])),
                "candidate_path": str(candidate) if refined else "",
                "candidate_sha256": candidate_sha,
                "candidate_changed": bool(candidate_sha and candidate_sha != result.get("checker_sha256")),
            }
        )
    if len(models) != 1 or len(wire_apis) != 1 or len(reasoning_efforts) != 1:
        raise ValueError("Baseline batch has inconsistent model or wire identity")
    manifest = {
        "schema_version": 1,
        "method": "strict_postvalidation_of_actual_knighter_refinement",
        "baseline_batch_result_sha256": sha256(batch_result_path),
        "validator_sha256": sha256(VALIDATOR),
        "review_policy": args.review_policy,
        "subjects": len(cases),
        "cases": cases,
    }
    manifest_path = output_dir / "STRICT_MANIFEST.json"
    if manifest_path.exists():
        previous = json.loads(manifest_path.read_text(encoding="utf-8"))
        if previous != manifest:
            raise RuntimeError("Existing strict-validation manifest differs from current candidates")
    else:
        write_json(manifest_path, manifest)
    if args.freeze_only:
        print(json.dumps({"subjects": len(cases), "upstream_refined": sum(row["upstream_refined"] for row in cases)}, indent=2))
        return 0

    rows = []
    for case in cases:
        case_id = case["case_id"]
        if not case["baseline_execution_valid"]:
            rows.append({"case_id": case_id, "status": "unscored_baseline_execution", "strict_pds": None, "llm_calls": case["llm_calls"]})
            continue
        if not case["upstream_refined"]:
            rows.append({"case_id": case_id, "status": "upstream_not_refined", "strict_pds": False, "llm_calls": case["llm_calls"]})
            continue
        if not case["candidate_sha256"]:
            rows.append({"case_id": case_id, "status": "missing_candidate", "strict_pds": False, "llm_calls": case["llm_calls"]})
            continue
        if not case["candidate_changed"]:
            rows.append({"case_id": case_id, "status": "unchanged_candidate", "strict_pds": False, "llm_calls": case["llm_calls"]})
            continue

        candidate = Path(case["candidate_path"])
        if sha256(candidate) != case["candidate_sha256"]:
            raise RuntimeError(f"Candidate drift before strict validation: {case_id}")
        case_output = output_dir / case_id
        result_path = case_output / "RESULT.json"
        if result_path.is_file():
            prior = json.loads(result_path.read_text(encoding="utf-8"))
            if (prior.get("candidate_sha256") == case["candidate_sha256"]
                    and prior.get("execution_valid")
                    and prior.get("review_policy") == args.review_policy):
                rows.append(
                    {
                        "case_id": case_id,
                        "status": "strict_pds" if prior.get("pds") else "strict_failure",
                        "strict_pds": bool(prior.get("pds")),
                        "pds_behavior": bool(prior.get("pds_behavior")),
                        "review_passed": bool((prior.get("artifact_review") or {}).get("passed")),
                        "adoptable_with_review_gate": bool(prior.get("adoptable_with_review_gate")),
                        "llm_calls": case["llm_calls"],
                        "result_sha256": sha256(result_path),
                        "new_model_calls": 0,
                    }
                )
                continue
        command = [
            sys.executable,
            str(VALIDATOR),
            "--candidate", str(candidate),
            "--case-dir", str(args.cases_root.resolve() / case_id),
            "--linux-dir", str(args.linux_dir.resolve()),
            "--output-dir", str(case_output),
            "--backend-workspace", str(backend_root / case_id),
            "--jobs", str(args.jobs),
            "--review-policy", args.review_policy,
        ]
        completed = subprocess.run(command, capture_output=True, text=True, check=False)
        case_output.mkdir(parents=True, exist_ok=True)
        (case_output / "validator.stdout.log").write_text(completed.stdout, encoding="utf-8")
        (case_output / "validator.stderr.log").write_text(completed.stderr, encoding="utf-8")
        result = json.loads(result_path.read_text(encoding="utf-8")) if result_path.is_file() else {}
        execution_valid = bool(result.get("execution_valid")) and completed.returncode == 0
        compile_failed = result.get("build_return_code") not in (None, 0)
        strict_pds = bool(result.get("pds")) if execution_valid else (False if compile_failed else None)
        rows.append(
            {
                "case_id": case_id,
                "status": (
                    "strict_pds" if strict_pds
                    else "candidate_compile_failure_external" if compile_failed
                    else "strict_failure" if execution_valid
                    else "strict_validation_error"
                ),
                "strict_pds": strict_pds,
                "pds_behavior": bool(result.get("pds_behavior")) if result else None,
                "review_passed": ((result.get("artifact_review") or {}).get("passed") if result else None),
                "adoptable_with_review_gate": bool(result.get("adoptable_with_review_gate")) if result else None,
                "llm_calls": case["llm_calls"],
                "result_sha256": sha256(result_path) if result_path.is_file() else "",
                "new_model_calls": 0,
            }
        )

    summary = {
        "schema_version": 1,
        "method": manifest["method"],
        "model": next(iter(models)),
        "wire_api": next(iter(wire_apis)),
        "reasoning_effort": next(iter(reasoning_efforts)),
        "strict_manifest_sha256": sha256(manifest_path),
        "subjects": len(rows),
        "strict_pds": sum(row["strict_pds"] is True for row in rows),
        "unscored": sum(row["strict_pds"] is None for row in rows),
        "review_failed_refined": sum(row.get("review_passed") is False for row in rows),
        "new_model_calls": 0,
        "baseline_model_calls": sum(case["llm_calls"] for case in cases),
        "rows": rows,
    }
    write_json(output_dir / "STRICT_RESULT.json", summary)
    print(json.dumps({key: value for key, value in summary.items() if key != "rows"}, indent=2))
    return 0 if summary["unscored"] == 0 else 2


if __name__ == "__main__":
    raise SystemExit(main())
