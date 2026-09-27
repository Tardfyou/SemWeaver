#!/usr/bin/env python3
"""Automatic model/CodeQL paired-feedback loop; development-only E2 probe."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path


HERE = Path(__file__).resolve().parent


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path: Path, payload: dict) -> None:
    path.write_text(json.dumps(payload, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def launch(command: list[str], stdout_path: Path, stderr_path: Path) -> int:
    completed = subprocess.run(command, capture_output=True, text=True, check=False)
    stdout_path.write_text(completed.stdout, encoding="utf-8")
    stderr_path.write_text(completed.stderr, encoding="utf-8")
    return completed.returncode


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--semweaver-root", required=True, type=Path)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--initial-query", required=True, type=Path)
    parser.add_argument("--patch", required=True, type=Path)
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--vulnerable-db", required=True, type=Path)
    parser.add_argument("--fixed-db", required=True, type=Path)
    parser.add_argument("--codeql", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--max-rounds", type=int, default=6)
    parser.add_argument("--model-calls-per-round", type=int, default=4)
    parser.add_argument("--total-model-call-cap", type=int, default=24)
    parser.add_argument("--max-tokens", type=int, default=16384)
    parser.add_argument("--query-timeout-seconds", type=int, default=300)
    parser.add_argument("--patch-window-radius", type=int, default=70)
    args = parser.parse_args()
    if min(args.max_rounds, args.model_calls_per_round, args.total_model_call_cap,
           args.max_tokens, args.query_timeout_seconds) <= 0:
        raise ValueError("All run budgets must be positive")
    if os.environ.get("SEMWEEVER_MODEL") != "gpt-6-luna":
        raise ValueError("This development protocol requires gpt-6-luna")
    if os.environ.get("SEMWEEVER_WIRE_API") != "responses":
        raise ValueError("This development protocol requires the Responses wire API")
    if os.environ.get("SEMWEEVER_REASONING_EFFORT") != "high":
        raise ValueError("This development protocol requires high reasoning effort")

    initial = args.initial_query.resolve()
    patch = args.patch.resolve()
    source = args.source_root.resolve()
    vuln_db = args.vulnerable_db.resolve()
    fixed_db = args.fixed_db.resolve()
    codeql = args.codeql.resolve()
    output = args.output_dir.resolve()
    for required in (initial, patch, initial.parent / "qlpack.yml", codeql,
                     vuln_db / "codeql-database.yml", fixed_db / "codeql-database.yml"):
        if not required.is_file():
            raise FileNotFoundError(required)
    if output.exists():
        raise FileExistsError(output)
    output.mkdir(parents=True)
    frozen = {
        "schema_version": 1,
        "method": "automatic_codeql_model_paired_feedback_loop_development",
        "model": "gpt-6-luna", "wire_api": "responses", "reasoning_effort": "high",
        "initial_query_sha256": sha256(initial),
        "patch_sha256": sha256(patch),
        "vulnerable_db_manifest_sha256": sha256(vuln_db / "codeql-database.yml"),
        "fixed_db_manifest_sha256": sha256(fixed_db / "codeql-database.yml"),
        "max_rounds": args.max_rounds,
        "model_calls_per_round": args.model_calls_per_round,
        "total_model_call_cap": args.total_model_call_cap,
        "max_tokens_per_call": args.max_tokens,
        "query_timeout_seconds_per_side": args.query_timeout_seconds,
        "patch_window_radius": args.patch_window_radius,
        "inference_boundary": "development-only; not part of 39-case CSA baseline",
    }
    write_json(output / "INPUT_MANIFEST.json", frozen)

    def paired_replay(query: Path, destination: Path) -> dict:
        command = [
            sys.executable, str(HERE / "validate_e2_codeql_pair.py"),
            "--codeql", str(codeql), "--query", str(query),
            "--vulnerable-db", str(vuln_db), "--fixed-db", str(fixed_db),
            "--output-dir", str(destination),
            "--timeout-seconds", str(args.query_timeout_seconds),
        ]
        launch(command, output / f"{destination.name}.stdout.log",
               output / f"{destination.name}.stderr.log")
        result_path = destination / "RESULT.json"
        if not result_path.is_file():
            raise RuntimeError(f"Paired replay failed before a result: {destination}")
        result = json.loads(result_path.read_text(encoding="utf-8"))
        if result.get("query_sha256") != sha256(query):
            raise RuntimeError("Paired replay query binding mismatch")
        return result

    baseline = paired_replay(initial, output / "initial_pair")
    rows = []
    result = {
        **frozen, "initial_pair_result_sha256": sha256(output / "initial_pair" / "RESULT.json"),
        "total_model_calls": 0, "completed_rounds": 0, "success": False,
        "status": "running", "rows": rows,
        "best_hit_recovery_with_fixed_noise": None,
    }
    if baseline.get("pds", False):
        result["status"] = "starting_query_already_pds"
        write_json(output / "RUN_RESULT.json", result)
        return 0
    if not baseline.get("execution_valid", False):
        result["status"] = "starting_query_execution_invalid"
        write_json(output / "RUN_RESULT.json", result)
        return 2

    current_query = initial
    current_feedback = output / "initial_pair" / "RESULT.json"
    unchanged_rounds = 0
    for index in range(1, args.max_rounds + 1):
        remaining = args.total_model_call_cap - result["total_model_calls"]
        if remaining <= 0:
            result["status"] = "model_call_cap_exhausted"
            break
        round_dir = output / f"round-{index:02d}"
        call_cap = min(remaining, args.model_calls_per_round)
        command = [
            sys.executable, str(HERE / "run_e2_codeql_development.py"),
            "--semweaver-root", str(args.semweaver_root.resolve()),
            "--config", str(args.config.resolve()),
            "--initial-query", str(current_query),
            "--feedback-result", str(current_feedback),
            "--patch", str(patch), "--source-root", str(source),
            "--vulnerable-db", str(vuln_db), "--fixed-db", str(fixed_db),
            "--output-dir", str(round_dir),
            "--max-iterations", str(max(4, call_cap)),
            "--max-model-calls", str(call_cap),
            "--max-tokens", str(args.max_tokens),
            "--patch-window-radius", str(args.patch_window_radius),
        ]
        return_code = launch(command, output / f"round-{index:02d}.stdout.log",
                             output / f"round-{index:02d}.stderr.log")
        manifest_path = round_dir / "RUN_MANIFEST.json"
        if return_code == 75 or not manifest_path.is_file():
            result["status"] = "provider_or_runner_interruption"
            write_json(output / "RUN_RESULT.json", result)
            return 75
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        calls = int((manifest.get("llm_usage", {}) or {}).get("call_count", 0) or 0)
        if calls > call_cap:
            raise RuntimeError("Per-round model-call cap exceeded")
        result["total_model_calls"] += calls
        query = round_dir / current_query.name
        changed = query.is_file() and sha256(query) != sha256(current_query)
        row = {
            "round": index, "input_query_sha256": sha256(current_query),
            "candidate_query_sha256": sha256(query) if query.is_file() else "",
            "candidate_changed": changed,
            "model_calls": calls,
            "agent_success": bool(manifest.get("agent_success", False)),
            "agent_manifest_sha256": sha256(manifest_path),
            "paired_result_sha256": "",
            "paired_execution_valid": None,
            "vulnerable_rows": None, "fixed_rows": None, "pds": False,
        }
        if changed:
            paired_path = output / f"round-{index:02d}-pair"
            paired = paired_replay(query, paired_path)
            row.update({
                "paired_result_sha256": sha256(paired_path / "RESULT.json"),
                "paired_execution_valid": bool(paired.get("execution_valid", False)),
                "vulnerable_rows": paired.get("vulnerable_rows"),
                "fixed_rows": paired.get("fixed_rows"),
                "pds": bool(paired.get("pds", False)),
            })
            if paired.get("execution_valid", False) and int(paired.get("vulnerable_rows") or 0) > 0:
                progress = {
                    "round": index,
                    "candidate_query_sha256": sha256(query),
                    "paired_result_sha256": sha256(paired_path / "RESULT.json"),
                    "vulnerable_rows": int(paired["vulnerable_rows"]),
                    "fixed_rows": int(paired["fixed_rows"]),
                    "pds": bool(paired.get("pds", False)),
                }
                previous = result["best_hit_recovery_with_fixed_noise"]
                if previous is None or (
                    progress["fixed_rows"], -progress["vulnerable_rows"], index
                ) < (
                    previous["fixed_rows"], -previous["vulnerable_rows"], previous["round"]
                ):
                    result["best_hit_recovery_with_fixed_noise"] = progress
            current_query = query
            current_feedback = paired_path / "RESULT.json"
            unchanged_rounds = 0
        else:
            unchanged_rounds += 1
        rows.append(row)
        result["completed_rounds"] = len(rows)
        if row["pds"] and row["agent_success"]:
            result["success"] = True
            result["status"] = "automatic_paired_pds"
            write_json(output / "RUN_RESULT.json", result)
            return 0
        if unchanged_rounds >= 2:
            result["status"] = "two_consecutive_no_change_rounds"
            break
        write_json(output / "RUN_RESULT.json", result)
    if result["status"] == "running":
        result["status"] = "round_cap_exhausted"
    if result["best_hit_recovery_with_fixed_noise"] is not None and not result["success"]:
        result["strict_pds_status"] = result["status"]
        result["status"] = "partial_target_hit_recovered_with_fixed_noise"
    write_json(output / "RUN_RESULT.json", result)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
