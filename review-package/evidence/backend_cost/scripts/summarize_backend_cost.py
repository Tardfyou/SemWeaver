#!/usr/bin/env python3
"""Join measured execution cost, logged setup intervals, and model-run records."""
import argparse
import datetime
import hashlib
import json
import re
import shutil
from pathlib import Path


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-root", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    data = args.data_root.resolve()
    output = args.output_dir.resolve()
    if output.exists():
        raise FileExistsError(output)
    output.mkdir(parents=True)
    e2 = data / "e2_imagemagick_development_v8"
    probe_path = data / "backend_cost_20260927_v1/RESULT.json"
    probe = json.loads(probe_path.read_text())
    assert probe["completed"] == 12 and probe["all_execution_valid"] and probe["all_outcomes_match"]
    setup = []
    for side in ("vulnerable", "fixed"):
        logs = list((e2 / f"codeql_db/416_{side}/log").glob("database-create-*.log"))
        assert len(logs) == 1
        log = logs[0]
        text = log.read_text()
        times = re.findall(r"^\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})\]", text, re.M)
        assert "Successfully created database" in text and "Terminating normally." in text
        assert "--build-mode=none" in text and "--threads=8" in text
        seconds = (datetime.datetime.fromisoformat(times[-1]) - datetime.datetime.fromisoformat(times[0])).total_seconds()
        relative = f"setup/{side}-database-create.log"
        (output / "setup").mkdir(exist_ok=True)
        shutil.copy2(log, output / relative)
        setup.append({"side": side, "log": relative, "log_sha256": sha(log), "observed_log_interval_seconds": seconds, "timestamp_resolution_seconds": 1, "note": "Log states it started late; interval excludes unlogged startup and is not complete install/setup time."})
    model_runs = []
    for name in ("auto_loop_416_v10", "auto_loop_416_v10_r2", "auto_loop_416_v10_r3"):
        root = e2 / name
        manifests = []
        for path in sorted(root.glob("round-*/RUN_MANIFEST.json")):
            row = json.loads(path.read_text())
            manifests.append({"path": str(path.relative_to(data)), "sha256": sha(path), "elapsed_seconds": row["elapsed_seconds"], "model_calls": row["llm_usage"]["call_count"], "total_tokens": row["llm_usage"]["total_tokens"]})
        result = json.loads((root / "RUN_RESULT.json").read_text())
        calls = sum(r["model_calls"] for r in manifests)
        assert calls == result["total_model_calls"]
        model_runs.append({"run": name, "result_sha256": sha(root / "RUN_RESULT.json"), "round_manifests": manifests, "model_calls": calls, "total_tokens": sum(r["total_tokens"] for r in manifests), "agent_round_seconds": sum(r["elapsed_seconds"] for r in manifests), "note": "Agent-round duration includes its tools; excludes external paired replay. Not pure API latency or end-to-end wall time."})
    payload = {"schema_version": 1, "replay_result_sha256": sha(probe_path), "replay_summary": probe["summary"], "database_setup": setup, "retained_model_runs": model_runs, "historical_developer_hours": None, "scope": "One existing CodeQL development case; deployment/setup observations, implementation surface, model cost and warm replay cost are distinct; no unseen-backend labor or whole-project throughput estimate."}
    (output / "RESULT.json").write_text(json.dumps(payload, indent=2) + "\n")
    print(json.dumps(payload, indent=2))


if __name__ == "__main__":
    main()
