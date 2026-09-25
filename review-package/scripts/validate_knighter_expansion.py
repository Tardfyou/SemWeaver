#!/usr/bin/env python3
"""Pair-validate every materialized KNighter expansion checker without an LLM.

The ten repaired-source substitutions are all retained in the manifest. A
compile failure, missing Linux revision, scanner failure, or absent target hit
is never silently promoted to a patch-local success. This screening result is
separate from the frozen 39-checker manuscript cohort until explicitly adopted.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--materialized-root", type=Path, required=True)
    parser.add_argument("--validator", type=Path, required=True)
    parser.add_argument("--linux-dir", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--backend-root", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--freeze-only", action="store_true")
    args = parser.parse_args()
    source_root = args.materialized_root.resolve()
    output_root = args.output_root.resolve()
    backend_root = args.backend_root.resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    backend_root.mkdir(parents=True, exist_ok=True)
    source_manifest_path = source_root / "MATERIALIZATION_MANIFEST.json"
    source = load(source_manifest_path)
    assert source["case_count"] == len(source["cases"]) == 10
    assert source["status"] == "materialized_unvalidated_not_a_study_denominator"
    frozen = []
    for row in source["cases"]:
        case_id = row["case_id"]
        case_dir = source_root / "cases" / case_id
        checker = case_dir / "csa" / "SAGenTestChecker.cpp"
        patch = case_dir / "patches" / "knighter_patch.md"
        metadata = case_dir / "metadata" / "candidate.json"
        assert all(path.is_file() for path in (checker, patch, metadata))
        assert sha(checker) == row["checker_sha256"]
        assert sha(patch) == row["knighter_patch_sha256"]
        assert sha(metadata) == row["metadata_sha256"]
        frozen.append({
            "case_id": case_id,
            "commit_id": row["commit_id"],
            "checker_sha256": row["checker_sha256"],
            "patch_sha256": row["knighter_patch_sha256"],
            "metadata_sha256": row["metadata_sha256"],
        })
    assert len({row["case_id"] for row in frozen}) == 10
    manifest = {
        "schema_version": 1,
        "status": "frozen_before_expansion_validation",
        "source_manifest_sha256": sha(source_manifest_path),
        "validator_sha256": sha(args.validator.resolve()),
        "linux_repo": str(args.linux_dir.resolve()),
        "jobs": args.jobs,
        "case_count": 10,
        "cases": frozen,
    }
    manifest_path = output_root / "VALIDATION_MANIFEST.json"
    if manifest_path.exists():
        if load(manifest_path) != manifest:
            raise RuntimeError("Existing expansion validation manifest differs")
    else:
        manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    if args.freeze_only:
        print(json.dumps({"cases": 10, "manifest_sha256": sha(manifest_path)}, indent=2))
        return 0

    rows = []
    for index, item in enumerate(frozen, start=1):
        case_id = item["case_id"]
        case_dir = source_root / "cases" / case_id
        case_output = output_root / case_id
        case_output.mkdir(parents=True, exist_ok=True)
        command = [
            sys.executable, str(args.validator.resolve()),
            "--candidate", str(case_dir / "csa" / "SAGenTestChecker.cpp"),
            "--case-dir", str(case_dir),
            "--linux-dir", str(args.linux_dir.resolve()),
            "--output-dir", str(case_output),
            "--backend-workspace", str(backend_root / case_id),
            "--jobs", str(args.jobs),
        ]
        print(f"[{index}/10] validate {case_id}", flush=True)
        completed = subprocess.run(command, capture_output=True, text=True, check=False)
        stdout = case_output / "validator.stdout.log"
        stderr = case_output / "validator.stderr.log"
        stdout.write_text(completed.stdout, encoding="utf-8")
        stderr.write_text(completed.stderr, encoding="utf-8")
        result_path = case_output / "RESULT.json"
        result = load(result_path) if result_path.is_file() else {}
        valid = bool(result.get("execution_valid", False)) and completed.returncode == 0
        vuln = result.get("vulnerable_alerts") if valid else None
        fixed = result.get("fixed_alerts") if valid else None
        if not valid:
            status = "execution_invalid_unscored"
        elif int(vuln) == 0:
            status = "vulnerable_target_miss"
        elif int(fixed) > 0:
            status = "fixed_noisy_refinable"
        else:
            status = "already_pds"
        rows.append({
            "case_id": case_id,
            "status": status,
            "execution_valid": valid,
            "vulnerable_alerts": vuln,
            "fixed_alerts": fixed,
            "validator_return_code": completed.returncode,
            "result_sha256": sha(result_path) if result_path.is_file() else "",
            "stdout_sha256": sha(stdout),
            "stderr_sha256": sha(stderr),
        })
        partial = {
            "schema_version": 1,
            "method": "independent_knighter_expansion_patch_local_screen",
            "validation_manifest_sha256": sha(manifest_path),
            "requested_cases": 10,
            "completed_records": len(rows),
            "rows": rows,
        }
        (output_root / "VALIDATION_RESULT.json").write_text(
            json.dumps(partial, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        print(f"[{index}/10] {status} V={vuln} F={fixed}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
