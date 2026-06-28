#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import json
import shlex
import shutil
import subprocess
import sys
import time
from collections import defaultdict
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[3]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from src.research.knighter_env import (  # type: ignore
    build_knighter_checker,
    build_knighter_process_env,
    extract_commit_id_from_patch,
    knighter_scan_prefix,
    load_knighter_e2_config,
    prepare_knighter_e2_scan_build,
)


DEFAULT_OUT = ROOT / "artifacts/experiments/knighter/e2/generalization/fullscan"
DEFAULT_ARCHIVE = ROOT / "artifacts/experiments/knighter/e2/generalization/reports"
DEFAULT_CONFIG = {
    "enabled": True,
    "knighter_root": str(ROOT / "experiments/knighter/baseline"),
    "llvm_dir": str(ROOT / "artifacts/external/llvm"),
    "linux_dir": str(ROOT / "artifacts/external/linux"),
    "host_deps_dir": str(ROOT / "artifacts/external/host_deps/jammy-amd64/root"),
    "utility_header": str(ROOT / "experiments/knighter/baseline/llvm_utils/utility.h"),
    "utility_source": str(ROOT / "experiments/knighter/baseline/llvm_utils/utility.cpp"),
    "result_dir": str(ROOT / "artifacts/experiments/knighter/runs"),
    "arch": "x86",
    "jobs": 8,
    "timeout": 3600,
    "scan_commit": "v6.13",
    "checker_name": "SAGenTest",
}


CASES = {
    "08_768f17fd25e4_Integer_Overflow": {
        "commit_id": "768f17fd25e4a98bf5166148629ecf6f647d5efc",
        "bug_type": "Integer-Overflow",
        "historical_report_count": 1729,
        "case_dir": "artifacts/experiments/knighter/e2/cases/08_768f17fd25e4_Integer_Overflow",
        "patch": "artifacts/experiments/knighter/e2/cases/08_768f17fd25e4_Integer_Overflow/patches/commit.patch",
        "refined_checker": "artifacts/experiments/knighter/e2/cases/08_768f17fd25e4_Integer_Overflow/csa/refinements/20260617_142234/csa/SAGenTestChecker.cpp",
    },
    "14_c3d749609472_Out_of_Bound": {
        "commit_id": "c3d749609472ba0b217b42ab66f80459847e2bcb",
        "bug_type": "Out-of-Bound",
        "historical_report_count": 6,
        "case_dir": "artifacts/experiments/knighter/e2/cases/14_c3d749609472_Out_of_Bound",
        "patch": "artifacts/experiments/knighter/e2/cases/14_c3d749609472_Out_of_Bound/patches/commit.patch",
        "refined_checker": "artifacts/experiments/knighter/e2/cases/14_c3d749609472_Out_of_Bound/csa/refinements/20260617_161108/csa/SAGenTestChecker.cpp",
    },
}


def rel(path: str | Path) -> str:
    target = Path(path).expanduser().resolve()
    try:
        return str(target.relative_to(ROOT))
    except ValueError:
        return str(target)


def run_cmd(cmd: str, *, cwd: Path, env: dict[str, str], log_file: Path, timeout: int) -> str:
    proc = subprocess.run(
        cmd,
        cwd=str(cwd),
        shell=True,
        env=env,
        text=True,
        capture_output=True,
        timeout=timeout,
    )
    output = (proc.stdout or "") + ("\n" + proc.stderr if proc.stderr else "")
    with log_file.open("a", encoding="utf-8") as handle:
        handle.write(f"$ {cmd}\n{output}\n")
    if proc.returncode != 0:
        raise RuntimeError(f"command failed ({proc.returncode}): {cmd}")
    return output


def newest_scan_output(run_dir: Path) -> Path | None:
    if not run_dir.exists():
        return None
    candidates = [p for p in run_dir.iterdir() if p.is_dir() and p.name[:4].isdigit()]
    return sorted(candidates)[-1] if candidates else None


def collect_reports(report_root: Path | None) -> dict[str, Any]:
    grouped: dict[str, list[str]] = defaultdict(list)
    if not report_root or not report_root.exists():
        return {"report_count": 0, "unique_report_count": 0, "report_dir": "", "grouped_reports": {}}
    reports = [p for p in report_root.rglob("*.html") if p.name != "index.html"]
    for report in reports:
        html = report.read_text(encoding="utf-8", errors="ignore")
        title = report.stem
        for line in html.splitlines()[:100]:
            if "<title>" in line and "</title>" in line:
                title = line.split("<title>", 1)[1].split("</title>", 1)[0]
                break
        grouped[title].append(rel(report))
    return {
        "report_count": len(reports),
        "unique_report_count": len(grouped),
        "report_dir": rel(report_root),
        "grouped_reports": dict(sorted(grouped.items())),
    }


def archive_report_dir(src: Path | None, archive_root: Path, case_id: str) -> str:
    if not src or not src.exists():
        return ""
    dst = archive_root / case_id / "fixed"
    if dst.exists():
        shutil.rmtree(dst)
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(src, dst)
    return rel(dst)


def run_case(env, case_id: str, output_root: Path, archive_root: Path) -> dict[str, Any]:
    meta = CASES[case_id]
    case_dir = ROOT / meta["case_dir"]
    checker_source = ROOT / meta["refined_checker"]
    patch_path = ROOT / meta["patch"]
    commit_id = extract_commit_id_from_patch(patch_path.read_text(errors="ignore")) or meta["commit_id"]
    case_output = output_root / case_id
    case_output.mkdir(parents=True, exist_ok=True)
    log_file = case_output / "refined_fixed_fullscan.log"
    log_file.write_text("", encoding="utf-8")

    ok, _, build_meta = build_knighter_checker(
        env,
        checker_source.read_text(encoding="utf-8", errors="ignore"),
        output_dir=str(case_output),
    )
    if not ok:
        return {
            "case_id": case_id,
            "commit_id": meta["commit_id"],
            "bug_type": meta["bug_type"],
            "success": False,
            "phase": "build_checker",
            "error": str(build_meta.get("error", "checker build failed")),
        }

    scan_build_path = prepare_knighter_e2_scan_build(env, case_dir)
    process_env = build_knighter_process_env(env)
    timeout = max(600, env.timeout)
    fixed_dir = case_output / "fixed"
    fixed_dir.mkdir(parents=True, exist_ok=True)

    started = time.time()
    run_cmd("make clean", cwd=env.linux_dir, env=process_env, log_file=log_file, timeout=timeout)
    run_cmd(f"git checkout {shlex.quote(commit_id)}", cwd=env.linux_dir, env=process_env, log_file=log_file, timeout=120)
    run_cmd(f"make LLVM=1 ARCH={env.arch} allyesconfig", cwd=env.linux_dir, env=process_env, log_file=log_file, timeout=timeout)
    prefix = knighter_scan_prefix(env, fixed_dir, scan_build_path=scan_build_path)
    run_cmd(prefix + f"make LLVM=1 ARCH={env.arch} olddefconfig", cwd=env.linux_dir, env=process_env, log_file=log_file, timeout=timeout)
    run_cmd(prefix + f"make LLVM=1 ARCH={env.arch} -j{max(1, env.jobs)}", cwd=env.linux_dir, env=process_env, log_file=log_file, timeout=max(3600, env.timeout))

    report_dir = newest_scan_output(fixed_dir)
    reports = collect_reports(report_dir)
    archived = archive_report_dir(Path(report_dir) if report_dir else None, archive_root, case_id)
    return {
        "case_id": case_id,
        "commit_id": meta["commit_id"],
        "bug_type": meta["bug_type"],
        "historical_report_count": meta["historical_report_count"],
        "success": True,
        "elapsed_sec": round(time.time() - started, 3),
        "checker_source": rel(checker_source),
        "log_file": rel(log_file),
        "archived_report_dir": archived,
        **reports,
    }


def write_summary(rows: list[dict[str, Any]], path: Path) -> None:
    fields = [
        "case_id",
        "commit_id",
        "bug_type",
        "historical_report_count",
        "success",
        "report_count",
        "unique_report_count",
        "elapsed_sec",
        "checker_source",
        "log_file",
        "archived_report_dir",
        "error",
    ]
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--case-id", choices=sorted(CASES), action="append")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--archive", type=Path, default=DEFAULT_ARCHIVE)
    parser.add_argument("--jobs", type=int, default=None)
    parser.add_argument("--timeout", type=int, default=None)
    args = parser.parse_args()

    raw = dict(DEFAULT_CONFIG)
    if args.jobs is not None:
        raw["jobs"] = args.jobs
    if args.timeout is not None:
        raw["timeout"] = args.timeout
    env = load_knighter_e2_config({"knighter_e2": raw})
    selected = args.case_id or sorted(CASES)
    all_results: list[dict[str, Any]] = []
    for index, case_id in enumerate(selected, start=1):
        print(f"[generalization-fullscan] {index}/{len(selected)} {case_id}", flush=True)
        result = run_case(env, case_id, args.output, args.archive)
        result_path = args.output / case_id / "refined_fixed_fullscan_result.json"
        result_path.write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        all_results.append(result)
        write_summary(all_results, args.output / "refined_fixed_fullscan_summary.csv")
        print(
            "[generalization-fullscan] "
            f"success={result.get('success')} reports={result.get('unique_report_count', '')}",
            flush=True,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
