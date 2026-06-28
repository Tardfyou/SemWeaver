#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import json
import re
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
MR = ROOT / "artifacts/experiments/knighter/e2/model_robustness"
SAMPLES = ROOT / "artifacts/experiments/knighter/e2/ablation/config/ablation_samples.csv"
RESULTS = MR / "results/model_robustness_results.csv"

FIELDNAMES = [
    "model_id",
    "case_id",
    "commit_id",
    "bug_type",
    "status",
    "baseline_buggy_alerts",
    "baseline_fixed_alerts",
    "gpt55_full_buggy_alerts",
    "gpt55_full_fixed_alerts",
    "refined_buggy_alerts",
    "refined_fixed_alerts",
    "buggy_delta_vs_baseline",
    "fixed_delta_vs_baseline",
    "matches_gpt55_pds",
    "strict_count_source",
    "strict_buggy_objects",
    "strict_fixed_objects",
    "strict_bad_marker_count",
    "run_dir",
    "checker_path",
    "final_report_path",
    "notes",
    "updated_at",
]


def parse_counts(log: Path) -> tuple[dict[str, dict[str, int]], int]:
    txt = log.read_text(errors="ignore")
    counts: dict[str, dict[str, int]] = {"buggy": {}, "fixed": {}}
    side = None
    obj = None
    for line in txt.splitlines():
        if line.startswith("$ ") and "scan-build " in line and " make LLVM=1 ARCH=x86 " in line:
            if "/buggy " in line or "/buggy/" in line:
                side = "buggy"
            elif "/fixed " in line or "/fixed/" in line:
                side = "fixed"
            m = re.search(r"make LLVM=1 ARCH=x86\s+([^\s]+\.o)\s+-j", line)
            obj = m.group(1) if m else None
        elif obj and line.startswith("scan-build:"):
            m = re.search(r"scan-build:\s*(\d+)\s+bugs? found", line, re.I)
            if m and side:
                counts[side][obj] = int(m.group(1))
                obj = None
            elif "No bugs found" in line and side:
                counts[side][obj] = 0
                obj = None
    bad = sum(
        txt.count(marker)
        for marker in [
            "Unknown command line argument",
            "analyzer encountered problems",
            "/failures",
        ]
    )
    return counts, bad


def read_samples() -> dict[str, dict[str, str]]:
    with SAMPLES.open(newline="") as fh:
        return {row["case_id"]: row for row in csv.DictReader(fh)}


def read_existing() -> list[dict[str, str]]:
    if not RESULTS.exists():
        return []
    with RESULTS.open(newline="") as fh:
        return list(csv.DictReader(fh))


def write_rows(rows: list[dict[str, str]]) -> None:
    RESULTS.parent.mkdir(parents=True, exist_ok=True)
    rows = sorted(rows, key=lambda r: (r.get("model_id", ""), r.get("case_id", "")))
    with RESULTS.open("w", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=FIELDNAMES)
        writer.writeheader()
        for row in rows:
            writer.writerow({name: row.get(name, "") for name in FIELDNAMES})


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--model-id", required=True)
    ap.add_argument("--case-id", required=True)
    ap.add_argument("--run-dir", required=True)
    ap.add_argument("--strict-log", required=True)
    ap.add_argument("--status", default="completed")
    ap.add_argument("--notes", default="")
    args = ap.parse_args()

    sample = read_samples()[args.case_id]
    run_dir = Path(args.run_dir)
    log = Path(args.strict_log)
    counts, bad = parse_counts(log)
    buggy = sum(counts["buggy"].values())
    fixed = sum(counts["fixed"].values())
    baseline_buggy = int(sample["baseline_buggy_alerts"])
    baseline_fixed = int(sample["baseline_fixed_alerts"])
    gpt_buggy = int(sample["full_evidence_refined_buggy_alerts"])
    gpt_fixed = int(sample["full_evidence_refined_fixed_alerts"])
    run_id = run_dir.name
    checker_candidates = [
        MR / "runs" / args.model_id / args.case_id / "csa" / "refinements" / run_id / "csa" / "SAGenTestChecker.cpp",
        run_dir / "csa" / "SAGenTestChecker.cpp",
        MR / "runs" / args.model_id / args.case_id / "csa" / "SAGenTestChecker.cpp",
    ]
    checker = next((p for p in checker_candidates if p.exists()), checker_candidates[0])
    final_report = run_dir / "final_report.json"

    row = {
        "model_id": args.model_id,
        "case_id": args.case_id,
        "commit_id": sample["commit_id"],
        "bug_type": sample["bug_type"],
        "status": args.status,
        "baseline_buggy_alerts": str(baseline_buggy),
        "baseline_fixed_alerts": str(baseline_fixed),
        "gpt55_full_buggy_alerts": str(gpt_buggy),
        "gpt55_full_fixed_alerts": str(gpt_fixed),
        "refined_buggy_alerts": str(buggy),
        "refined_fixed_alerts": str(fixed),
        "buggy_delta_vs_baseline": str(buggy - baseline_buggy),
        "fixed_delta_vs_baseline": str(fixed - baseline_fixed),
        "matches_gpt55_pds": str(buggy == gpt_buggy and fixed == gpt_fixed),
        "strict_count_source": str(log),
        "strict_buggy_objects": json.dumps(counts["buggy"], sort_keys=True),
        "strict_fixed_objects": json.dumps(counts["fixed"], sort_keys=True),
        "strict_bad_marker_count": str(bad),
        "run_dir": str(run_dir),
        "checker_path": str(checker),
        "final_report_path": str(final_report if final_report.exists() else ""),
        "notes": args.notes,
        "updated_at": datetime.now().isoformat(timespec="seconds"),
    }

    rows = [
        r for r in read_existing()
        if not (r.get("model_id") == args.model_id and r.get("case_id") == args.case_id)
    ]
    rows.append(row)
    write_rows(rows)
    print(f"{args.model_id},{args.case_id}: strict {buggy}/{fixed}, bad_markers={bad}")


if __name__ == "__main__":
    main()
