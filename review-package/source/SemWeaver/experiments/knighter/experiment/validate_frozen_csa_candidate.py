#!/usr/bin/env python3
"""Deterministically compile, review, and pair-validate a frozen CSA candidate."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import sys
from pathlib import Path


HERE = Path(__file__).resolve().parent
PROJECT_ROOT = HERE.parents[2]
BASELINE_SRC = PROJECT_ROOT / "experiments" / "knighter" / "baseline" / "src"
sys.path.insert(0, str(PROJECT_ROOT))
sys.path.insert(0, str(BASELINE_SRC))
sys.path.insert(0, str(HERE))

from global_config import logger  # noqa: E402
from targets.linux import Linux  # noqa: E402

from packaged_clang_backend import PackagedClangBackend  # noqa: E402
from src.tools.artifact_review import ArtifactReviewTool  # noqa: E402


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def score_candidate(
    *, execution_valid: bool, vulnerable_alerts: int | None,
    fixed_alerts: int | None, review_passed: bool, review_policy: str,
) -> dict[str, bool]:
    """Keep paired behavior and structural review as distinct dimensions."""
    if review_policy not in ("diagnostic", "gate"):
        raise ValueError(f"Unsupported review policy: {review_policy}")
    vulnerable_hit = execution_valid and bool(vulnerable_alerts and vulnerable_alerts > 0)
    fixed_silent = execution_valid and fixed_alerts == 0
    pds_behavior = vulnerable_hit and fixed_silent
    return {
        "vulnerable_hit": vulnerable_hit,
        "fixed_silent": fixed_silent,
        "pds_behavior": pds_behavior,
        "pds": pds_behavior and (review_policy == "diagnostic" or review_passed),
        "adoptable_with_review_gate": pds_behavior and review_passed,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate", required=True, type=Path)
    parser.add_argument("--case-dir", required=True, type=Path)
    parser.add_argument("--linux-dir", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--backend-workspace", required=True, type=Path)
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument(
        "--review-policy", choices=("diagnostic", "gate"), default="diagnostic",
        help="Keep structural review findings separate from paired execution by default",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    candidate = args.candidate.resolve()
    case_dir = args.case_dir.resolve()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    run_log = output_dir / "VALIDATION_RUN.log"
    sink = logger.add(run_log, level="DEBUG")

    metadata = json.loads(
        (case_dir / "metadata" / "candidate.json").read_text(encoding="utf-8")
    )
    patch_path = case_dir / "patches" / "knighter_patch.md"
    checker_code = candidate.read_text(encoding="utf-8")
    backend = PackagedClangBackend(
        str(args.backend_workspace.resolve()),
        cmake_project=str(
            PROJECT_ROOT / "experiments" / "robustness" / "case07_negative_control"
        ),
        utility_source=str(
            PROJECT_ROOT / "experiments" / "knighter" / "baseline" / "llvm_utils" / "utility.cpp"
        ),
        utility_header=str(
            PROJECT_ROOT / "experiments" / "knighter" / "baseline" / "llvm_utils" / "utility.h"
        ),
    )
    target = Linux(str(args.linux_dir.resolve()))
    required = ("git", "cmake", "ninja", "make", "clang", "clang++", "scan-build-18")
    runtime_tools = {tool: shutil.which(tool) or "" for tool in required}
    missing = [tool for tool, path in runtime_tools.items() if not path]
    if missing:
        raise RuntimeError(f"Missing runtime tools: {', '.join(missing)}")

    findings, warnings = ArtifactReviewTool()._review_csa_refine(checker_code)
    build_rc, build_stderr = backend.build_checker(
        checker_code,
        output_dir / "build_logs",
        attempt=1,
        jobs=args.jobs,
        timeout=300,
    )
    vulnerable_counts = {}
    fixed_counts = {}
    # A static quality finding is not an analyzer execution failure. The
    # primary PDS oracle runs the checker whenever it compiles; review remains
    # a separately reported adoption/quality dimension. Legacy gate behavior
    # is retained explicitly for reproduction of older diagnostics.
    if build_rc == 0 and (args.review_policy == "diagnostic" or not findings):
        objects = target.get_objects_from_patch(patch_path.read_text(encoding="utf-8"))
        if not objects:
            raise RuntimeError("No C build objects resolved from the frozen patch")
        for object_path in objects:
            safe_object = object_path.replace("/", "-").removesuffix(".o")
            vulnerable_counts[object_path] = backend.run_checker(
                checker_code,
                commit_id=f"{metadata['commit_id']}^",
                target=target,
                object_to_analyze=object_path,
                jobs=args.jobs,
                output_dir=output_dir / "vulnerable" / safe_object,
                skip_build_checker=True,
                timeout=1800,
            )
            fixed_counts[object_path] = backend.run_checker(
                checker_code,
                commit_id=str(metadata["commit_id"]),
                target=target,
                object_to_analyze=object_path,
                jobs=args.jobs,
                output_dir=output_dir / "fixed" / safe_object,
                skip_build_checker=True,
                timeout=1800,
            )

    logger.remove(sink)
    log_text = run_log.read_text(encoding="utf-8", errors="ignore")
    infrastructure_patterns = (
        "Failed to run allyesconfig",
        "clang: not found",
        "command not found",
        "No rule to make target",
        "Error in build",
    )
    infrastructure_errors = [
        pattern for pattern in infrastructure_patterns if pattern in log_text
    ]
    execution_valid = (
        build_rc == 0
        and bool(vulnerable_counts)
        and all(value >= 0 for value in vulnerable_counts.values())
        and bool(fixed_counts)
        and all(value >= 0 for value in fixed_counts.values())
        and not infrastructure_errors
    )
    vulnerable_alerts = sum(vulnerable_counts.values()) if vulnerable_counts else None
    fixed_alerts = sum(fixed_counts.values()) if fixed_counts else None
    scores = score_candidate(
        execution_valid=execution_valid,
        vulnerable_alerts=vulnerable_alerts,
        fixed_alerts=fixed_alerts,
        review_passed=not findings,
        review_policy=args.review_policy,
    )
    result = {
        "schema_version": 1,
        "method": "frozen_csa_candidate_paired_validation",
        "case_id": case_dir.name,
        "commit_id": str(metadata["commit_id"]),
        "candidate_path": str(candidate),
        "candidate_sha256": sha256(candidate),
        "patch_sha256": sha256(patch_path),
        "runtime_tools": runtime_tools,
        "artifact_review": {
            "findings": findings,
            "warnings": warnings,
            "passed": not findings,
        },
        "review_policy": args.review_policy,
        "pds_behavior": scores["pds_behavior"],
        "adoptable_with_review_gate": scores["adoptable_with_review_gate"],
        "build_return_code": build_rc,
        "build_stderr_tail": build_stderr[-2000:] if build_stderr else "",
        "vulnerable_object_counts": vulnerable_counts,
        "fixed_object_counts": fixed_counts,
        "vulnerable_alerts": vulnerable_alerts,
        "fixed_alerts": fixed_alerts,
        "vulnerable_hit": scores["vulnerable_hit"],
        "fixed_silent": scores["fixed_silent"],
        "execution_valid": execution_valid,
        "pds": scores["pds"],
        "infrastructure_errors": infrastructure_errors,
        "new_llm_calls": 0,
    }
    (output_dir / "RESULT.json").write_text(
        json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    print(json.dumps(result, indent=2, ensure_ascii=False))
    return 0 if execution_valid else 2


if __name__ == "__main__":
    raise SystemExit(main())
