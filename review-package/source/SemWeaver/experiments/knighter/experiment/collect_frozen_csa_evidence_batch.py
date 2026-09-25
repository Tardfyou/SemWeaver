#!/usr/bin/env python3
"""Collect provenance-gated CSA evidence for a frozen Knighter cohort."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path


HERE = Path(__file__).resolve().parent
PROJECT_ROOT = HERE.parents[2]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def cohort_case_ids(payload: dict) -> list[str]:
    ids = []
    for entry in list(payload.get("cases", []) or []):
        case_id = str(entry.get("case_id", "") if isinstance(entry, dict) else entry)
        if case_id and case_id not in ids:
            ids.append(case_id)
    return ids


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--cases-root", required=True, type=Path)
    parser.add_argument("--cohort-manifest", required=True, type=Path)
    parser.add_argument("--linux-dir", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--jobs", type=int, default=8)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    cases_root = args.cases_root.resolve()
    cohort_path = args.cohort_manifest.resolve()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    source_cohort = json.loads(cohort_path.read_text(encoding="utf-8"))
    case_ids = cohort_case_ids(source_cohort)
    if not case_ids:
        raise ValueError("cohort manifest has no cases")

    frozen_cases = []
    for case_id in case_ids:
        case_dir = cases_root / case_id
        paths = {
            "source_plan": case_dir / "patchweaver_plan.json",
            "patch": case_dir / "patches" / "commit.patch",
            "checker": case_dir / "csa" / "SAGenTestChecker.cpp",
            "metadata": case_dir / "metadata" / "candidate.json",
        }
        missing = [str(path) for path in paths.values() if not path.is_file()]
        if missing:
            raise FileNotFoundError(", ".join(missing))
        frozen_cases.append({
            "case_id": case_id,
            **{f"{name}_sha256": sha256(path) for name, path in paths.items()},
        })
    revision = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=str(PROJECT_ROOT),
        capture_output=True,
        text=True,
        check=True,
    ).stdout.strip()
    batch_manifest = {
        "schema_version": 1,
        "method": "frozen_plan_csa_evidence_replay_batch",
        "source_cohort_manifest_sha256": sha256(cohort_path),
        "implementation_revision": revision,
        "case_count": len(frozen_cases),
        "execution_order": case_ids,
        "cases": frozen_cases,
    }
    batch_manifest_path = output_dir / "BATCH_MANIFEST.json"
    batch_manifest_path.write_text(
        json.dumps(batch_manifest, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )

    rows = []
    driver = HERE / "collect_frozen_csa_evidence.py"
    for index, case_id in enumerate(case_ids, start=1):
        case_output = output_dir / case_id
        command = [
            sys.executable,
            str(driver),
            "--config",
            str(args.config.resolve()),
            "--case-dir",
            str(cases_root / case_id),
            "--linux-dir",
            str(args.linux_dir.resolve()),
            "--output-dir",
            str(case_output),
            "--prepare-vulnerable",
            "--jobs",
            str(args.jobs),
        ]
        completed = subprocess.run(command, capture_output=True, text=True, check=False)
        (output_dir / f"coordinator-{index:02d}-{case_id}.stdout.log").write_text(
            completed.stdout, encoding="utf-8"
        )
        (output_dir / f"coordinator-{index:02d}-{case_id}.stderr.log").write_text(
            completed.stderr, encoding="utf-8"
        )
        result_path = case_output / "EVIDENCE_REPLAY_MANIFEST.json"
        if result_path.is_file():
            result = json.loads(result_path.read_text(encoding="utf-8"))
            row = {
                "case_id": case_id,
                "return_code": completed.returncode,
                "execution_valid": True,
                "eligible": bool(result.get("eligible", False)),
                "decision": str(result.get("decision", "")),
                "records": int(result.get("records", 0) or 0),
                "origin_counts": dict(result.get("origin_counts", {}) or {}),
                "result_sha256": sha256(result_path),
            }
        else:
            row = {
                "case_id": case_id,
                "return_code": completed.returncode,
                "execution_valid": False,
                "eligible": False,
                "decision": "evidence_replay_error",
                "records": 0,
                "origin_counts": {},
                "result_sha256": "",
            }
        rows.append(row)
        partial = {
            "schema_version": 1,
            "method": "frozen_plan_csa_evidence_replay_batch",
            "batch_manifest_sha256": sha256(batch_manifest_path),
            "requested_cases": len(case_ids),
            "completed_records": len(rows),
            "execution_valid": sum(row["execution_valid"] for row in rows),
            "eligible": sum(row["eligible"] for row in rows),
            "abstained": sum(
                row["decision"] == "abstain_missing_analyzer_internal" for row in rows
            ),
            "errors": sum(not row["execution_valid"] for row in rows),
            "rows": rows,
        }
        (output_dir / "BATCH_RESULT.json").write_text(
            json.dumps(partial, indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8",
        )

    print(json.dumps(partial, indent=2, ensure_ascii=False))
    return 0 if partial["errors"] == 0 else 2


if __name__ == "__main__":
    raise SystemExit(main())
