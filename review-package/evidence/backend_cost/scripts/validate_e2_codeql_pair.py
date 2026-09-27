#!/usr/bin/env python3
"""Independently replay one unchanged CodeQL query on a frozen source pair."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import signal
import subprocess
from pathlib import Path


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _run_with_group_timeout(command: list[str], timeout_seconds: int):
    """Ensure a timed-out CodeQL launcher does not leave a DB-locking JVM."""
    process = subprocess.Popen(
        command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, start_new_session=True,
    )
    try:
        stdout, stderr = process.communicate(timeout=timeout_seconds)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.communicate()
        return None
    return subprocess.CompletedProcess(command, process.returncode, stdout, stderr)


def run_query(codeql: Path, query: Path, database: Path, output: Path, side: str,
              timeout_seconds: int) -> dict:
    bqrs = output / f"{side}.bqrs"
    command = [
        str(codeql), "query", "run", f"--database={database}",
        f"--output={bqrs}", str(query),
    ]
    executed = _run_with_group_timeout(command, timeout_seconds)
    if executed is None:
        return {"valid": False, "rows": None, "reason": "query_timeout",
                "timeout_seconds": timeout_seconds}
    if executed.returncode:
        return {
            "valid": False, "rows": None, "exit_code": executed.returncode,
            "diagnostic_tail": (executed.stdout + executed.stderr)[-4000:],
        }
    decoded = _run_with_group_timeout(
        [str(codeql), "bqrs", "decode", "--format=json", "--entities=all", str(bqrs)],
        120,
    )
    if decoded is None:
        return {"valid": False, "rows": None, "reason": "decode_timeout",
                "timeout_seconds": 120}
    if decoded.returncode:
        return {
            "valid": False, "rows": None, "exit_code": decoded.returncode,
            "diagnostic_tail": (decoded.stdout + decoded.stderr)[-4000:],
        }
    tables = json.loads(decoded.stdout)
    selected = tables.get("#select", {})
    tuples = selected.get("tuples", [])
    return {
        "valid": True, "rows": len(tuples), "bqrs_sha256": sha256(bqrs),
        "sample_rows": tuples[:5],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--codeql", required=True, type=Path)
    parser.add_argument("--query", required=True, type=Path)
    parser.add_argument("--vulnerable-db", required=True, type=Path)
    parser.add_argument("--fixed-db", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--timeout-seconds", type=int, default=600)
    args = parser.parse_args()
    if args.timeout_seconds <= 0:
        raise ValueError("timeout-seconds must be positive")
    codeql = args.codeql.resolve()
    query = args.query.resolve()
    vulnerable = args.vulnerable_db.resolve()
    fixed = args.fixed_db.resolve()
    output = args.output_dir.resolve()
    for required in (codeql, query, vulnerable / "codeql-database.yml", fixed / "codeql-database.yml"):
        if not required.is_file():
            raise FileNotFoundError(required)
    if output.exists():
        raise FileExistsError(output)
    output.mkdir(parents=True)
    vulnerable_result = run_query(codeql, query, vulnerable, output, "vulnerable", args.timeout_seconds)
    fixed_result = (
        run_query(codeql, query, fixed, output, "fixed", args.timeout_seconds)
        if vulnerable_result["valid"] else
        {"valid": False, "rows": None, "reason": "skipped_after_vulnerable_execution_failure"}
    )
    sides = {"vulnerable": vulnerable_result, "fixed": fixed_result}
    valid = all(side["valid"] for side in sides.values())
    payload = {
        "schema_version": 1,
        "method": "independent_frozen_codeql_pair_replay",
        "query_sha256": sha256(query),
        "query_timeout_seconds_per_side": args.timeout_seconds,
        "vulnerable_db_manifest_sha256": sha256(vulnerable / "codeql-database.yml"),
        "fixed_db_manifest_sha256": sha256(fixed / "codeql-database.yml"),
        "execution_valid": valid,
        "vulnerable_rows": sides["vulnerable"]["rows"],
        "fixed_rows": sides["fixed"]["rows"],
        "pds": bool(valid and sides["vulnerable"]["rows"] > 0 and sides["fixed"]["rows"] == 0),
        "sides": sides,
    }
    (output / "RESULT.json").write_text(json.dumps(payload, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps({key: payload[key] for key in (
        "execution_valid", "vulnerable_rows", "fixed_rows", "pds",
    )}, indent=2))
    return 0 if valid else 2


if __name__ == "__main__":
    raise SystemExit(main())
