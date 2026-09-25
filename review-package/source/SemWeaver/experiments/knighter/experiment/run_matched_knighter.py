#!/usr/bin/env python3
"""Run Knighter's actual refinement loop on one frozen SemWeaver E1 subject."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import sys
from pathlib import Path

from html2text import html2text


HERE = Path(__file__).resolve().parent
PROJECT_ROOT = HERE.parents[2]
BASELINE_SRC = PROJECT_ROOT / "experiments" / "knighter" / "baseline" / "src"
sys.path.insert(0, str(BASELINE_SRC))
sys.path.insert(0, str(HERE))

from checker_data import CheckerData  # noqa: E402
from checker_refine import refine_checker_with_max_attempts  # noqa: E402
from global_config import global_config, logger  # noqa: E402
from model import init_llm, usage_log  # noqa: E402
from targets.linux import Linux  # noqa: E402
from tools import remove_text_section  # noqa: E402

from packaged_clang_backend import PackagedClangBackend  # noqa: E402
from report_binding import canonical_build_source_from_html  # noqa: E402


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case-dir", required=True, type=Path)
    parser.add_argument("--fixed-report-dir", required=True, type=Path)
    parser.add_argument("--linux-dir", required=True, type=Path)
    parser.add_argument("--output-root", required=True, type=Path)
    parser.add_argument("--backend-workspace", required=True, type=Path)
    parser.add_argument("--model", default="gpt-5.6-terra")
    parser.add_argument("--provider", default="custom", choices=("custom",))
    parser.add_argument("--wire-api", default="responses", choices=("responses", "chat_completions"))
    parser.add_argument("--reasoning-effort", default="medium")
    parser.add_argument("--temperature", type=float, default=0.0)
    parser.add_argument("--max-tokens", type=int, default=16000)
    parser.add_argument("--max-tries", type=int, default=1)
    parser.add_argument("--max-fp-reports", type=int, default=5)
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--timeout", type=int, default=1800)
    parser.add_argument("--dry-run", action="store_true")
    return parser.parse_args()


def fixed_reports(path: Path, limit: int):
    reports = []
    seen_files = set()
    file_pattern = re.compile(r"BuildSource:\| (.+)")
    for report_path in sorted(path.glob("report-*.html")):
        html = report_path.read_text(encoding="utf-8", errors="ignore")
        markdown = remove_text_section(html2text(html), html)
        source_path = canonical_build_source_from_html(html)
        markdown = f"BuildSource:| {source_path}\n{markdown}"
        match = file_pattern.search(markdown)
        source_file = match.group(1).strip() if match else report_path.stem
        if source_file in seen_files:
            continue
        seen_files.add(source_file)
        reports.append({"id": report_path.stem, "content": markdown})
        if len(reports) >= limit:
            break
    return reports


def runtime_preflight() -> dict[str, str]:
    required = ("git", "cmake", "ninja", "make", "clang", "clang++", "scan-build-18")
    resolved = {tool: shutil.which(tool) or "" for tool in required}
    missing = [tool for tool, path in resolved.items() if not path]
    if missing:
        raise RuntimeError(f"Missing required runtime tools: {', '.join(missing)}")
    return resolved


def prepare_checker(case_dir: Path, output_root: Path) -> CheckerData:
    metadata = json.loads((case_dir / "metadata" / "candidate.json").read_text(encoding="utf-8"))
    checker_index_match = re.search(r"checker(\d+)$", str(metadata["checker_id"]))
    if not checker_index_match:
        raise ValueError(f"Cannot parse checker index: {metadata['checker_id']}")

    checker = CheckerData(
        commit_id=str(metadata["commit_id"]),
        commit_type=str(metadata["bug_type"]),
        base_result_dir=output_root,
        index=int(checker_index_match.group(1)),
    )
    checker.patch = (case_dir / "patches" / "knighter_patch.md").read_text(encoding="utf-8")
    checker.pattern = (case_dir / "metadata" / "pattern.txt").read_text(encoding="utf-8")
    checker.plan = (case_dir / "metadata" / "plan.txt").read_text(encoding="utf-8")
    checker.refined_plan = (case_dir / "metadata" / "refined_plan.txt").read_text(encoding="utf-8")
    checker.initial_checker_code = (case_dir / "csa" / "SAGenTestChecker.cpp").read_text(encoding="utf-8")
    checker.repaired_checker_code = checker.initial_checker_code
    checker.tp_score = int(metadata.get("tp", 1) or 1)
    checker.tn_score = int(metadata.get("tn", 1) or 1)
    checker.dump_dir()
    return checker


def configure_runtime(args: argparse.Namespace, checker: CheckerData) -> PackagedClangBackend:
    backend = PackagedClangBackend(
        str(args.backend_workspace.resolve()),
        cmake_project=str(PROJECT_ROOT / "experiments" / "robustness" / "case07_negative_control"),
        utility_source=str(PROJECT_ROOT / "experiments" / "knighter" / "baseline" / "llvm_utils" / "utility.cpp"),
        utility_header=str(PROJECT_ROOT / "experiments" / "knighter" / "baseline" / "llvm_utils" / "utility.h"),
    )
    target = Linux(str(args.linux_dir.resolve()))
    global_config._config = {
        "result_dir": str(args.output_root.resolve()),
        "linux_dir": str(args.linux_dir.resolve()),
        "LLVM_dir": str(args.backend_workspace.resolve()),
        "target_type": "linux",
        "target": target,
        "backend": backend,
        "scan_commit": checker.commit_id,
        "jobs": args.jobs,
        "scan_timeout": args.timeout,
        "max_fp_reports_for_refinement": args.max_fp_reports,
        "model": args.model,
        "temperature": args.temperature,
        "max_tokens": args.max_tokens,
    }
    custom_key = os.environ.get("CUSTOM_OPENAI_API_KEY", "")
    custom_base_url = os.environ.get("CUSTOM_OPENAI_BASE_URL", "")
    if (not custom_key or not custom_base_url) and not args.dry_run:
        raise RuntimeError(
            "CUSTOM_OPENAI_API_KEY and CUSTOM_OPENAI_BASE_URL are required for "
            "this matched-baseline profile."
        )
    os.environ["CUSTOM_OPENAI_WIRE_API"] = args.wire_api
    os.environ["CUSTOM_OPENAI_REASONING_EFFORT"] = args.reasoning_effort
    global_config._keys = {}
    global_config._initialized = True
    return backend


def main() -> int:
    args = parse_args()
    case_dir = args.case_dir.resolve()
    output_root = args.output_root.resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    checker = prepare_checker(case_dir, output_root)
    run_dir = Path(checker.output_dir)
    run_log = run_dir / "MATCHED_BASELINE_RUN.log"
    log_sink = logger.add(run_log, level="DEBUG")
    reports = fixed_reports(args.fixed_report_dir.resolve(), args.max_fp_reports)
    if not reports:
        raise RuntimeError("No fixed-side scan-build HTML reports found.")
    backend = configure_runtime(args, checker)

    preflight = {} if args.dry_run else runtime_preflight()
    manifest = {
        "schema_version": 1,
        "method": "knighter_actual_refinement_loop_packaged_clang_adapter",
        "case_id": case_dir.name,
        "commit_id": checker.commit_id,
        "checker_id": checker.checker_id,
        "checker_sha256": sha256(case_dir / "csa" / "SAGenTestChecker.cpp"),
        "patch_sha256": sha256(case_dir / "patches" / "knighter_patch.md"),
        "fixed_report_count_available": len(list(args.fixed_report_dir.resolve().glob("report-*.html"))),
        "fixed_report_count_used": len(reports),
        "fixed_report_ids": [report["id"] for report in reports],
        "model": args.model,
        "provider": args.provider,
        "wire_api": args.wire_api,
        "reasoning_effort": args.reasoning_effort,
        "temperature": args.temperature,
        "max_tokens": args.max_tokens,
        "max_tries": args.max_tries,
        "max_fp_reports": args.max_fp_reports,
        "jobs": args.jobs,
        "timeout": args.timeout,
        "intervention": "automatic",
        "adapter_scope": "plugin compilation and packaged tool paths only",
        "runtime_tools": preflight,
    }
    (run_dir / "RUN_MANIFEST.json").write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    if args.dry_run:
        print(json.dumps(manifest, indent=2, ensure_ascii=False))
        return 0

    init_llm()
    try:
        results = refine_checker_with_max_attempts(
            checker,
            scan=False,
            max_tries=args.max_tries,
            timeout=args.timeout,
            reports_override=reports,
        )
    finally:
        logger.remove(log_sink)
    skipped_responses = sorted(
        str(path)
        for path in run_dir.glob("prompt_history/**/response_*.md")
        if "SKIP" in path.read_text(encoding="utf-8", errors="ignore")
    )
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
        not skipped_responses and bool(usage_log) and not infrastructure_errors
    )
    summary = {
        **manifest,
        "results": [
            {
                "attempt_id": result.attempt_id,
                "result": result.result,
                "refined": result.refined,
                "num_TP": result.num_TP,
                "num_FP": result.num_FP,
                "num_reports": result.num_reports,
            }
            for result in results
        ],
        "llm_usage": list(usage_log),
        "execution_valid": execution_valid,
        "infrastructure_errors": infrastructure_errors,
        "skipped_response_paths": skipped_responses,
        "plugin_path": str(backend.plugin_path),
        "completed": True,
    }
    (run_dir / "MATCHED_BASELINE_RESULT.json").write_text(
        json.dumps(summary, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(summary, indent=2, ensure_ascii=False))
    return 0 if execution_valid else 2


if __name__ == "__main__":
    raise SystemExit(main())
