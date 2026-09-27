#!/usr/bin/env python3
"""Resume deterministic validation of a frozen Knighter LLM candidate.

This entry point is intentionally model-free. It is for a run whose LLM calls
and syntax compilation completed, but whose validation was interrupted by an
adapter or toolchain failure. The exact candidate bytes are hashed before the
corrected validation path runs.
"""

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
sys.path.insert(0, str(BASELINE_SRC))
sys.path.insert(0, str(HERE))

from global_config import logger  # noqa: E402
from targets.linux import Linux  # noqa: E402

from packaged_clang_backend import PackagedClangBackend  # noqa: E402


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case-dir", required=True, type=Path)
    parser.add_argument("--source-run-dir", required=True, type=Path)
    parser.add_argument("--linux-dir", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--backend-workspace", required=True, type=Path)
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--timeout", type=int, default=1800)
    return parser.parse_args()


def preflight() -> dict[str, str]:
    required = ("git", "cmake", "ninja", "make", "clang", "clang++", "scan-build-18")
    resolved = {tool: shutil.which(tool) or "" for tool in required}
    missing = [tool for tool, path in resolved.items() if not path]
    if missing:
        raise RuntimeError(f"Missing required runtime tools: {', '.join(missing)}")
    return resolved


def main() -> int:
    args = parse_args()
    case_dir = args.case_dir.resolve()
    source_run_dir = args.source_run_dir.resolve()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    run_log = output_dir / "RESUMED_VALIDATION_RUN.log"
    sink = logger.add(run_log, level="DEBUG")

    metadata = json.loads(
        (case_dir / "metadata" / "candidate.json").read_text(encoding="utf-8")
    )
    source_manifest = json.loads(
        (source_run_dir / "RUN_MANIFEST.json").read_text(encoding="utf-8")
    )
    source_result = json.loads(
        (source_run_dir / "MATCHED_BASELINE_RESULT.json").read_text(encoding="utf-8")
    )
    candidate = source_run_dir / "refine" / "refine-0-0" / "syntax_correct_refine_code.cpp"
    report_dir = source_run_dir / "refine" / "refine-0-0"
    report_subdirs = sorted(path for path in report_dir.glob("report-*") if path.is_dir())
    if len(report_subdirs) != 1:
        raise RuntimeError(f"Expected one frozen report directory, found {len(report_subdirs)}")
    report_content = (report_subdirs[0] / "report_content.txt").read_text(encoding="utf-8")
    checker_code = candidate.read_text(encoding="utf-8")
    patch = (case_dir / "patches" / "knighter_patch.md").read_text(encoding="utf-8")

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
    objects = backend.get_objects_from_report(report_content, target)
    tools = preflight()

    build_rc, build_stderr = backend.build_checker(
        checker_code,
        output_dir / "build_logs",
        attempt=1,
        jobs=args.jobs,
        timeout=300,
    )
    fixed_bugs = None
    validation_tp = None
    validation_tn = None
    if build_rc == 0:
        fixed_bugs = backend.run_checker(
            checker_code,
            commit_id=str(metadata["commit_id"]),
            target=target,
            object_to_analyze=objects[0],
            jobs=args.jobs,
            output_dir=output_dir / "fixed_object_scan",
            skip_build_checker=True,
            timeout=args.timeout,
        )
        if fixed_bugs >= 0:
            validation_tp, validation_tn = backend.validate_checker(
                checker_code,
                str(metadata["commit_id"]),
                patch,
                target=target,
                skip_build_checker=True,
            )

    logger.remove(sink)
    run_log_text = run_log.read_text(encoding="utf-8", errors="ignore")
    infrastructure_patterns = (
        "Failed to run allyesconfig",
        "clang: not found",
        "command not found",
        "No rule to make target",
        "Fail to build the kernel with checker",
    )
    infrastructure_errors = [
        pattern for pattern in infrastructure_patterns if pattern in run_log_text
    ]
    execution_valid = (
        build_rc == 0
        and fixed_bugs is not None
        and fixed_bugs >= 0
        and validation_tp is not None
        and validation_tp >= 0
        and validation_tn is not None
        and validation_tn >= 0
        and not infrastructure_errors
    )
    accepted = (
        execution_valid
        and fixed_bugs == 0
        and validation_tp > 0
        and validation_tn > 0
    )
    response_hashes = {
        str(path.relative_to(source_run_dir)): sha256(path)
        for path in sorted(source_run_dir.glob("prompt_history/**/response_*.md"))
    }
    result = {
        "schema_version": 1,
        "method": "knighter_frozen_candidate_resumed_validation",
        "case_id": case_dir.name,
        "commit_id": str(metadata["commit_id"]),
        "source_run_manifest_sha256": sha256(source_run_dir / "RUN_MANIFEST.json"),
        "source_invalidated_result_sha256": sha256(
            source_run_dir / "MATCHED_BASELINE_RESULT.json"
        ),
        "candidate_path": str(candidate),
        "candidate_sha256": sha256(candidate),
        "response_sha256": response_hashes,
        "source_model": source_manifest["model"],
        "source_provider": source_manifest["provider"],
        "source_wire_api": source_manifest["wire_api"],
        "source_reasoning_effort": source_manifest["reasoning_effort"],
        "source_llm_usage": source_result["llm_usage"],
        "new_llm_calls": 0,
        "resolved_objects": objects,
        "adapter_correction": "fail-closed unique binding from report basename to frozen Linux source tree",
        "runtime_tools": tools,
        "build_return_code": build_rc,
        "build_stderr_tail": build_stderr[-2000:] if build_stderr else "",
        "fixed_side_bug_count": fixed_bugs,
        "paired_validation_tp": validation_tp,
        "paired_validation_tn": validation_tn,
        "execution_valid": execution_valid,
        "refinement_accepted": accepted,
        "infrastructure_errors": infrastructure_errors,
    }
    (output_dir / "RESUMED_VALIDATION_RESULT.json").write_text(
        json.dumps(result, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(result, indent=2, ensure_ascii=False))
    return 0 if execution_valid else 2


if __name__ == "__main__":
    raise SystemExit(main())
