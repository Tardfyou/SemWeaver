#!/usr/bin/env python3
"""Measure retained CodeQL replay cost; never edits detectors or calls a model."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import shutil
import signal
import statistics
import subprocess
import time
from pathlib import Path


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n")


def measured(command, directory, name, timeout=600):
    stats = directory / f"{name}.time.txt"
    started = time.perf_counter()
    with (directory / f"{name}.stdout.log").open("w") as out, (directory / f"{name}.stderr.log").open("w") as err:
        process = subprocess.Popen(["/usr/bin/time", "-f", "%e %U %S %M", "-o", str(stats), *command], stdout=out, stderr=err, start_new_session=True)
        try:
            rc = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            return {"execution_valid": False, "reason": "timeout", "wall_seconds": time.perf_counter() - started}
    payload = {"execution_valid": rc == 0, "exit_code": rc, "wall_seconds": time.perf_counter() - started, "command": command}
    if stats.exists():
        values = stats.read_text().strip().splitlines()[-1].split()
        if len(values) == 4:
            payload.update(zip(("time_wall_seconds", "user_seconds", "system_seconds", "max_rss_kib"), map(float, values)))
    return payload


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--codeql", required=True, type=Path)
    parser.add_argument("--vulnerable-db", required=True, type=Path)
    parser.add_argument("--fixed-db", required=True, type=Path)
    parser.add_argument("--initial-query", required=True, type=Path)
    parser.add_argument("--refined-query", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--repeats", default=3, type=int)
    args = parser.parse_args()
    if args.repeats < 1:
        raise ValueError("repeats must be positive")
    output = args.output_dir.resolve()
    if output.exists():
        raise FileExistsError(output)
    codeql = args.codeql.resolve()
    dbs = {"vulnerable": args.vulnerable_db.resolve(), "fixed": args.fixed_db.resolve()}
    queries = {"initial": args.initial_query.resolve(), "refined": args.refined_query.resolve()}
    for path in [codeql, *queries.values(), *(p.parent / "qlpack.yml" for p in queries.values()), *(p / "codeql-database.yml" for p in dbs.values())]:
        if not path.is_file():
            raise FileNotFoundError(path)
    output.mkdir(parents=True)
    version = subprocess.run([str(codeql), "version", "--format=json"], capture_output=True, text=True, check=True)
    write(output / "CODEQL_VERSION.json", json.loads(version.stdout))
    metadata_hashes = {side: sha(db / "codeql-database.yml") for side, db in dbs.items()}
    manifest = {
        "schema_version": 1, "method": "existing_database_codeql_cost_probe", "repeats": args.repeats,
        "query_hashes": {key: sha(path) for key, path in queries.items()},
        "pack_hashes": {key: sha(path.parent / "qlpack.yml") for key, path in queries.items()},
        "database_manifest_hashes": metadata_hashes, "driver_sha256": sha(Path(__file__)),
        "platform": platform.platform(), "visible_cpus": os.cpu_count(), "threads": 2, "ram_mb": 2048,
        "cache_policy": "Existing databases and shared library caches retained; a fresh query directory is used per repetition; not a cold-install benchmark.",
        "scope": "One existing CodeQL case; excludes database creation, model latency, developer labor and throughput extrapolation; repeated executions are not independent subjects.",
    }
    write(output / "INPUT_MANIFEST.json", manifest)
    rows = []
    for repeat in range(1, args.repeats + 1):
        # Alternate query order to avoid fixing one variant to the first slot.
        order = list(queries) if repeat % 2 else list(reversed(queries))
        for label in order:
            root = output / f"rep-{repeat:02d}-{label}"
            root.mkdir()
            query = root / queries[label].name
            shutil.copy2(queries[label], query)
            shutil.copy2(queries[label].parent / "qlpack.yml", root / "qlpack.yml")
            for side, db in dbs.items():
                command = [str(codeql), "query", "run", "--threads=2", "--ram=2048", f"--search-path={codeql.parent}", f"--database={db}", f"--output={root / (side + '.bqrs')}", str(query)]
                query_run = measured(command, root, f"{side}-query")
                decoded = measured([str(codeql), "bqrs", "decode", "--format=json", "--entities=all", str(root / (side + '.bqrs'))], root, f"{side}-decode", 120) if query_run["execution_valid"] else None
                valid = query_run["execution_valid"] and bool(decoded and decoded["execution_valid"])
                count = len(json.loads((root / f"{side}-decode.stdout.log").read_text())["#select"]["tuples"]) if valid else None
                expected = 8 if label == "refined" and side == "vulnerable" else 0
                row = {"repeat": repeat, "variant": label, "side": side, "query": query_run, "decode": decoded, "rows": count, "expected_rows": expected, "execution_valid": valid, "outcome_matches": valid and count == expected}
                rows.append(row)
                write(output / "PROGRESS.json", {"completed": len(rows), "requested": args.repeats * 4, "rows": rows})
                print(json.dumps({k: row[k] for k in ("repeat", "variant", "side", "rows", "execution_valid")}), flush=True)
                if not valid:
                    raise RuntimeError("Execution failure retained; not a measured successful replay")
    summary = {}
    for label in queries:
        pairs = []
        for repeat in range(1, args.repeats + 1):
            subset = [r for r in rows if r["repeat"] == repeat and r["variant"] == label]
            pairs.append(sum(r["query"]["wall_seconds"] + r["decode"]["wall_seconds"] for r in subset))
        rss = [r["query"]["max_rss_kib"] for r in rows if r["variant"] == label]
        summary[label] = {"paired_seconds": pairs, "median_paired_seconds": statistics.median(pairs), "min_paired_seconds": min(pairs), "max_paired_seconds": max(pairs), "peak_process_rss_mib": max(rss) / 1024}
    assert metadata_hashes == {side: sha(db / "codeql-database.yml") for side, db in dbs.items()}
    assert all(r["outcome_matches"] for r in rows)
    write(output / "RESULT.json", {"schema_version": 1, "input_manifest_sha256": sha(output / "INPUT_MANIFEST.json"), "completed": len(rows), "all_execution_valid": True, "all_outcomes_match": True, "summary": summary, "rows": rows})


if __name__ == "__main__":
    main()
