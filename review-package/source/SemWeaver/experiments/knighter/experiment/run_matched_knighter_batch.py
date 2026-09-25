#!/usr/bin/env python3
"""Run the matched Knighter profile sequentially on a frozen cohort CSV."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path


HERE = Path(__file__).resolve().parent
PROJECT_ROOT = HERE.parents[2]
SINGLE_RUNNER = HERE / "run_matched_knighter.py"

from report_binding import canonical_build_source_from_html


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def rate_limit_observed(log_text: str) -> bool:
    return bool(re.search(
        r"gateway_concurrency_limit|rate_limit_error|error code:\s*429|http\s*429",
        log_text,
        flags=re.IGNORECASE,
    ))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cohort-csv", required=True, type=Path)
    parser.add_argument("--cases-root", required=True, type=Path)
    parser.add_argument("--fixed-reports-root", required=True, type=Path)
    parser.add_argument("--linux-dir", required=True, type=Path)
    parser.add_argument("--output-root", required=True, type=Path)
    parser.add_argument("--backend-root", required=True, type=Path)
    parser.add_argument("--model", default="gpt-5.6-terra")
    parser.add_argument("--wire-api", default="responses", choices=("responses",))
    parser.add_argument("--reasoning-effort", default="medium")
    parser.add_argument("--max-fp-reports", type=int, default=5)
    parser.add_argument("--max-tries", type=int, default=1)
    parser.add_argument("--max-tokens", type=int, default=16000)
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--timeout", type=int, default=1800)
    parser.add_argument("--start-case")
    parser.add_argument("--stop-after", type=int)
    parser.add_argument("--freeze-only", action="store_true")
    return parser.parse_args()


def read_cohort(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8-sig") as handle:
        rows = list(csv.DictReader(handle))
    if not rows or any(not row.get("case_id") for row in rows):
        raise ValueError("Cohort CSV must contain non-empty case_id values")
    case_ids = [row["case_id"] for row in rows]
    if len(case_ids) != len(set(case_ids)):
        raise ValueError("Cohort CSV contains duplicate case_id values")
    return rows


def canonical_report_source(path: Path) -> str:
    html = path.read_text(encoding="utf-8", errors="ignore")
    try:
        return canonical_build_source_from_html(html)
    except ValueError as exc:
        raise ValueError(f"{exc}: {path}") from exc


def result_path(output_root: Path, case_dir: Path) -> Path:
    metadata = json.loads(
        (case_dir / "metadata" / "candidate.json").read_text(encoding="utf-8")
    )
    checker_id = str(metadata["checker_id"])
    bug_type = str(metadata["bug_type"])
    commit = str(metadata["commit_id"])[:8]
    index = checker_id.rsplit("checker", 1)[-1]
    return output_root / f"KN-{bug_type}-{commit}-{index}" / "MATCHED_BASELINE_RESULT.json"


def main() -> int:
    args = parse_args()
    cohort_path = args.cohort_csv.resolve()
    rows = read_cohort(cohort_path)
    if args.start_case:
        indices = [i for i, row in enumerate(rows) if row["case_id"] == args.start_case]
        if not indices:
            raise ValueError(f"Unknown --start-case: {args.start_case}")
        rows = rows[indices[0] :]
    if args.stop_after is not None:
        if args.stop_after <= 0:
            raise ValueError("--stop-after must be positive")
        rows = rows[: args.stop_after]

    output_root = args.output_root.resolve()
    backend_root = args.backend_root.resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    backend_root.mkdir(parents=True, exist_ok=True)

    frozen_cases = []
    for row in rows:
        case_id = row["case_id"]
        case_dir = args.cases_root.resolve() / case_id
        report_dir = args.fixed_reports_root.resolve() / case_id / "fixed"
        required = (
            case_dir / "metadata" / "candidate.json",
            case_dir / "csa" / "SAGenTestChecker.cpp",
            case_dir / "patches" / "knighter_patch.md",
        )
        missing = [str(path) for path in required if not path.is_file()]
        reports = sorted(report_dir.glob("report-*.html"))
        if missing or not reports:
            raise RuntimeError(
                f"Incomplete frozen input for {case_id}: missing={missing}, reports={len(reports)}"
            )
        report_sources = {path.name: canonical_report_source(path) for path in reports}
        for report_name, source in report_sources.items():
            verified = subprocess.run(
                [
                    "git",
                    "-C",
                    str(args.linux_dir.resolve()),
                    "cat-file",
                    "-e",
                    f"{row.get('commit_id')}:{source}",
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            if verified.returncode != 0:
                raise RuntimeError(
                    f"Report source not present at fixed commit for {case_id}/{report_name}: {source}"
                )
        frozen_cases.append(
            {
                "selection_rank": row.get("selection_rank"),
                "case_id": case_id,
                "commit_id": row.get("commit_id"),
                "checker_sha256": sha256(required[1]),
                "patch_sha256": sha256(required[2]),
                "fixed_report_count": len(reports),
                "fixed_report_sha256": {path.name: sha256(path) for path in reports},
                "fixed_report_source": report_sources,
            }
        )

    batch_manifest = {
        "schema_version": 1,
        "created_at": datetime.now(timezone.utc).isoformat(),
        "method": "upstream_knighter_actual_refinement_loop_packaged_clang_adapter",
        "cohort_csv_sha256": sha256(cohort_path),
        "case_count": len(frozen_cases),
        "model": args.model,
        "provider": "custom",
        "wire_api": args.wire_api,
        "reasoning_effort": args.reasoning_effort,
        "max_fp_reports": args.max_fp_reports,
        "max_tries": args.max_tries,
        "max_tokens": args.max_tokens,
        "jobs": args.jobs,
        "timeout": args.timeout,
        "execution_order": [case["case_id"] for case in frozen_cases],
        "cases": frozen_cases,
    }
    manifest_path = output_root / "BATCH_MANIFEST.json"
    if manifest_path.exists():
        previous = json.loads(manifest_path.read_text(encoding="utf-8"))
        comparable_previous = {key: value for key, value in previous.items() if key != "created_at"}
        comparable_current = {key: value for key, value in batch_manifest.items() if key != "created_at"}
        if comparable_previous != comparable_current:
            raise RuntimeError("Existing batch manifest does not match requested frozen run")
    else:
        manifest_path.write_text(
            json.dumps(batch_manifest, indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8",
        )

    if args.freeze_only:
        print(json.dumps(batch_manifest, indent=2, ensure_ascii=False))
        return 0
    if (output_root / "BATCH_INTERRUPTED.json").exists():
        raise RuntimeError("Rate-limited baseline batch must restart in a fresh output directory")

    case_results = []
    for position, case in enumerate(frozen_cases, start=1):
        case_id = case["case_id"]
        case_dir = args.cases_root.resolve() / case_id
        report_dir = args.fixed_reports_root.resolve() / case_id / "fixed"
        expected_result = result_path(output_root, case_dir)
        if expected_result.is_file():
            prior = json.loads(expected_result.read_text(encoding="utf-8"))
            if prior.get("execution_valid"):
                print(f"[{position}/{len(frozen_cases)}] skip valid {case_id}", flush=True)
                case_results.append(
                    {"case_id": case_id, "return_code": 0, "result": prior, "resumed": True}
                )
                continue

        command = [
            sys.executable,
            str(SINGLE_RUNNER),
            "--case-dir",
            str(case_dir),
            "--fixed-report-dir",
            str(report_dir),
            "--linux-dir",
            str(args.linux_dir.resolve()),
            "--output-root",
            str(output_root),
            "--backend-workspace",
            str(backend_root / case_id),
            "--model",
            args.model,
            "--provider",
            "custom",
            "--wire-api",
            args.wire_api,
            "--reasoning-effort",
            args.reasoning_effort,
            "--max-fp-reports",
            str(args.max_fp_reports),
            "--max-tries",
            str(args.max_tries),
            "--max-tokens",
            str(args.max_tokens),
            "--jobs",
            str(args.jobs),
            "--timeout",
            str(args.timeout),
        ]
        log_path = output_root / f"coordinator-{position:02d}-{case_id}.log"
        print(f"[{position}/{len(frozen_cases)}] start {case_id}", flush=True)
        with log_path.open("w", encoding="utf-8") as log_handle:
            process = subprocess.Popen(
                command,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1,
            )
            assert process.stdout is not None
            for line in process.stdout:
                log_handle.write(line)
                log_handle.flush()
                print(line, end="", flush=True)
            return_code = process.wait()

        run_log = log_path.read_text(encoding="utf-8", errors="ignore")
        if return_code == 75 or rate_limit_observed(run_log):
            interruption = {
                "schema_version": 1,
                "status": "batch_interrupted_before_method_outcome",
                "reason": "model_rate_limit",
                "batch_manifest_sha256": sha256(manifest_path),
                "case_id": case_id,
                "case_index": position,
                "completed_records": len(case_results),
                "coordinator_log_sha256": sha256(log_path),
            }
            (output_root / "BATCH_INTERRUPTED.json").write_text(
                json.dumps(interruption, indent=2, ensure_ascii=False) + "\n",
                encoding="utf-8",
            )
            print(json.dumps(interruption, indent=2, ensure_ascii=False), file=sys.stderr)
            return 75

        result = None
        if expected_result.is_file():
            result = json.loads(expected_result.read_text(encoding="utf-8"))
        case_results.append(
            {
                "case_id": case_id,
                "return_code": return_code,
                "result": result,
                "resumed": False,
            }
        )
        print(f"[{position}/{len(frozen_cases)}] end {case_id} rc={return_code}", flush=True)

    valid_results = [
        row["result"]
        for row in case_results
        if row["result"] and row["result"].get("execution_valid")
    ]
    summary = {
        "schema_version": 1,
        "batch_manifest_sha256": sha256(manifest_path),
        "requested_cases": len(frozen_cases),
        "completed_records": sum(row["result"] is not None for row in case_results),
        "execution_valid": len(valid_results),
        "execution_invalid": sum(
            row["result"] is not None and not row["result"].get("execution_valid")
            for row in case_results
        ),
        "missing_result": sum(row["result"] is None for row in case_results),
        "accepted_refinements": sum(
            any(attempt.get("refined") for attempt in result.get("results", []))
            for result in valid_results
        ),
        "cases": case_results,
    }
    (output_root / "BATCH_RESULT.json").write_text(
        json.dumps(summary, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    print(json.dumps({key: value for key, value in summary.items() if key != "cases"}, indent=2))
    return 0 if len(valid_results) == len(frozen_cases) else 2


if __name__ == "__main__":
    raise SystemExit(main())
