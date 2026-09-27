#!/usr/bin/env python3
"""Offline verification of scored references, not superseded success flags."""
import argparse
import csv
import json
import sys
from pathlib import Path
sys.dont_write_bytecode = True
from verify_fse2027_review_package import Package, require, load, verify_private_data, verify_inputs_and_evidence, verify_motivating_example, verify_backend_cost, verify_e2


def metrics(rows):
    tp = sum(r["vulnerable_alerts"] > 0 for r in rows)
    fp = sum(r["fixed_alerts"] > 0 for r in rows)
    fn = len(rows) - tp
    return {"cases": len(rows), "TP": tp, "FP": fp, "FN": fn, "TN": len(rows) - fp, "PDS": sum(r["vulnerable_alerts"] > 0 and r["fixed_alerts"] == 0 for r in rows), "precision": tp / (tp + fp) if tp + fp else 0, "recall": tp / len(rows) if rows else 0, "F1": 2 * tp / (2 * tp + fp + fn) if 2 * tp + fp + fn else 0, "fixed_object_alerts": sum(r["fixed_alerts"] for r in rows)}


def verify_row(package, row):
    path = Path("evidence/fse_revision") / row["validation_path"]
    package.bound(path, row["validation_sha256"])
    result = load(package.root / path)
    require(result["execution_valid"] is True, f"Invalid scored pair: {path}")
    require((row["vulnerable_alerts"], row["fixed_alerts"]) == (result["vulnerable_alerts"], result["fixed_alerts"]), f"Scored pair drift: {path}")
    expected = row.get("selected_candidate_sha256", row.get("candidate_sha256"))
    require(result["candidate_sha256"] == expected, f"Scored checker identity: {path}")
    require(expected in package.checker_digests, f"Scored checker source is absent: {path}")
    for failure in (package.root / path).parent.rglob("*"):
        if failure.is_file() and "failures" in failure.parts and failure.name.endswith((".info.txt", ".stderr.txt")):
            require(not failure.read_bytes().strip(), f"Scored analysis has failure artifact: {failure.relative_to(package.root)}")
    if "source_batch" in row:
        batch = Path("evidence/fse_revision") / row["source_batch"]
        package.bound(batch / "BATCH_RESULT.json", row["source_batch_result_sha256"])
        raw = next(r for r in load(package.root / batch / "BATCH_RESULT.json")["rows"] if r["case_id"] == row["case_id"])
        require(row["actual_model_calls"] == raw["llm_calls"], "Actual model call accounting")
        require(row["actual_model_calls"] <= row["call_cap"], "Per-case call ceiling")
        for attempt in row["candidate_attempts"]:
            events = package.root / batch / row["case_id"] / f"attempt-{attempt['attempt']:02d}/run_events.jsonl"
            if events.exists():
                require(not any(json.loads(line).get("event") == "agent_exception" for line in events.read_text().splitlines() if line.strip()), f"Unhandled agent exception in scored chain: {events.relative_to(package.root)}")
            if attempt.get("candidate_path"):
                package.bound(Path("evidence/fse_revision") / attempt["candidate_path"], attempt["candidate_sha256"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    args = parser.parse_args()
    package = Package(args.package)
    require(package.manifest["method"] == "fse2027_scan_corrected_review_package", "Wrong package type")
    verify_private_data(package)
    package.checker_digests = {package.original_sha(path.relative_to(package.root)) for directory in ("inputs", "evidence") for path in (package.root / directory).rglob("*.cpp")}
    ids, origins = verify_inputs_and_evidence(package)
    package.bound("results/corrected/RESULT.json", package.manifest["corrected_summary_sha256"])
    data = load(package.root / "results/corrected/RESULT.json")
    full = {"unmodified": data["starting"], "knighter": data["knighter"], **data["portfolios"]}
    for name, block in full.items():
        require({r["case_id"] for r in block["rows"]} == set(ids), f"Full-cohort identity: {name}")
        require(metrics(block["rows"]) == block["metrics"], f"Metric arithmetic: {name}")
        for row in block["rows"]:
            verify_row(package, row)
    precision = {r["case_id"] for r in data["starting"]["rows"] if r["vulnerable_alerts"] > 0 and r["fixed_alerts"] > 0}
    require(len(precision) == 15, "Corrected precision denominator")
    baseline_rows = [r for r in data["knighter"]["rows"] if r["case_id"] in precision]
    baseline_calls = {r["case_id"]: r["model_calls"] for r in baseline_rows}
    require(metrics(baseline_rows) == data["matched15"]["knighter"]["metrics"], "Matched baseline metrics")
    require(sum(baseline_calls.values()) == data["matched15"]["knighter"]["actual_model_calls"], "Matched baseline calls")
    for mode in ("native", "no_internal"):
        block = data["matched15"][mode]
        require({r["case_id"] for r in block["rows"]} == precision, "Matched cases")
        require(metrics(block["rows"]) == block["metrics"], "Matched metrics")
        require(sum(r["actual_model_calls"] for r in block["rows"]) == block["actual_model_calls"], "Matched total calls")
        for row in block["rows"]:
            require(row["call_cap"] == baseline_calls[row["case_id"]], "Same per-case KNighter call ceiling")
            verify_row(package, row)
    repeated_ids = None
    for key, repeats in data["repeated12"].items():
        require(len(repeats) == 3, f"Repeat count: {key}")
        for repeat in repeats:
            require(len(repeat["rows"]) == 12, "Repeated subject count")
            observed_ids = {r["case_id"] for r in repeat["rows"]}
            if repeated_ids is None:
                repeated_ids = observed_ids
            require(len(observed_ids) == 12 and observed_ids == repeated_ids, "Frozen repeat identities")
            for name, expected in (("precision", 5), ("recovery", 7)):
                rows = [r for r in repeat["rows"] if (r["case_id"] in precision) == (name == "precision")]
                require(len(rows) == expected and metrics(rows) == repeat[name], "Repeat strata/arithmetic")
            for row in repeat["rows"]:
                verify_row(package, row)
    with (package.root / "results/corrected/SUMMARY.csv").open() as stream:
        csv_rows = list(csv.DictReader(stream))
    require({r["method"] for r in csv_rows} == set(full), "Summary CSV methods")
    require(len(csv_rows) == len(full), "Summary CSV uniqueness")
    for row in csv_rows:
        require(all(float(row[k]) == value for k, value in full[row["method"]]["metrics"].items()), "Summary CSV arithmetic")
    example = verify_motivating_example(package)
    cost = verify_backend_cost(package)
    e2 = verify_e2(package)
    print(json.dumps({"status": "verified_corrected_package", "cases": 39, "precision_cases": 15, "recovery_cases": 24, "evidence_origins": dict(origins), "portfolio_PDS": {k: v["metrics"]["PDS"] for k, v in full.items()}, "motivating_example": example, "backend_cost": cost, "codeql": e2}, indent=2))


if __name__ == "__main__":
    main()
