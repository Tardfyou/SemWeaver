#!/usr/bin/env python3
"""Offline integrity, provenance, and outcome verifier for the 39-case package."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
import statistics
from collections import Counter, defaultdict
from pathlib import Path


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def sha(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


class Package:
    def __init__(self, root: Path) -> None:
        self.root = root.resolve()
        manifest = load(self.root / "ARTIFACT_MANIFEST.json")
        require(manifest["case_count"] == 39, "manifest case denominator")
        require(manifest["file_count"] == len(manifest["files"]), "manifest file count")
        expected: set[str] = set()
        for row in manifest["files"]:
            relative = str(row["path"])
            require(relative not in expected, f"duplicate manifest entry: {relative}")
            expected.add(relative)
            path = self.root / relative
            require(path.is_file() and not path.is_symlink(), f"missing or linked file: {relative}")
            require(path.resolve().is_relative_to(self.root), f"manifest path escape: {relative}")
            require(path.stat().st_size == row["size"], f"file size mismatch: {relative}")
            require(sha(path) == row["sha256"], f"file hash mismatch: {relative}")
        actual = {
            path.relative_to(self.root).as_posix()
            for path in self.root.rglob("*")
            if path.is_file() and path.relative_to(self.root).as_posix() != "ARTIFACT_MANIFEST.json"
        }
        require(actual == expected, f"unmanifested or missing files: {len(actual ^ expected)}")
        redactions = load(self.root / "REDACTIONS.json")
        require(redactions["file_count"] == len(redactions["files"]), "redaction count")
        self.redactions = {row["path"]: row for row in redactions["files"]}
        require(len(self.redactions) == len(redactions["files"]), "duplicate redaction")
        for relative, row in self.redactions.items():
            require(relative in expected, f"orphan redaction: {relative}")
            require(sha(self.root / relative) == row["redacted_sha256"], f"redacted hash: {relative}")
        self.manifest = manifest

    def original_sha(self, relative: str | Path) -> str:
        relative = Path(relative).as_posix()
        path = self.root / relative
        require(path.is_file(), f"bound file missing: {relative}")
        row = self.redactions.get(relative)
        return str(row["original_sha256"]) if row else sha(path)

    def bound(self, relative: str | Path, expected: str) -> None:
        relative = Path(relative).as_posix()
        require(self.original_sha(relative) == expected, f"source digest mismatch: {relative}")


def verify_private_data(package: Package) -> None:
    key = re.compile(rb"(?:sk-[A-Za-z0-9]{20,}|olp_[A-Za-z0-9]{20,}|\b[a-f0-9]{32}\.[A-Za-z0-9]{16,}\b)")
    # Generic public checks must not embed the private identifiers they audit.
    # The publisher separately checks its private account/project strings.
    private_home = re.compile(rb"(?<!/anonymous)/(?:home|Users)/(?!anonymous(?:/|[\s\"']))[A-Za-z0-9_.-]+/")
    private_gateway = re.compile(rb"https?://(?:[0-9]{1,3}\.){3}[0-9]{1,3}:8080")
    for path in package.root.rglob("*"):
        if not path.is_file():
            continue
        data = path.read_bytes()
        relative = path.relative_to(package.root)
        require(not key.search(data), f"possible private credential in {relative}")
        require(not private_home.search(data) and not private_gateway.search(data), f"identity or gateway string in {relative}")


def verify_inputs_and_evidence(package: Package) -> tuple[list[str], Counter[str]]:
    selection = load(package.root / "inputs/generate_only_v1/SELECTION_MANIFEST.json")
    rows = selection["cases"]
    require(len(rows) == 39, "selection denominator")
    ids = [str(row["case_id"]) for row in rows]
    require(len(set(ids)) == 39, "selection duplicate case")
    inventory = selection["inventory"]
    require(inventory["generated_rows"] == 286 and inventory["generate_only_rows"] == 241, "upstream inventory")
    require(inventory["generate_only_commits"] == 39, "upstream generate-only commits")

    evidence_root = Path("evidence/generate_only_evidence_all39_v8_r5")
    manifest = load(package.root / evidence_root / "BATCH_MANIFEST.json")
    batch = load(package.root / evidence_root / "BATCH_RESULT.json")
    audit = load(package.root / evidence_root / "STRICT_BINDING_AUDIT.json")
    require(manifest["case_count"] == batch["requested_cases"] == batch["completed_records"] == 39, "evidence replay completeness")
    require({row["case_id"] for row in manifest["cases"]} == set(ids), "evidence subjects")
    require(audit["passed"] and audit["subjects"] == audit["passed_subjects"] == 39, "binding audit")
    require(audit["bound_eligible_subjects"] == 38 and audit["audited_abstentions"] == 1, "internal evidence coverage")
    audit_rows = {row["case_id"]: row for row in audit["rows"]}
    origins: Counter[str] = Counter()
    for row in manifest["cases"]:
        case_id = str(row["case_id"])
        input_root = Path("inputs/cases") / case_id
        for key_name, filename in (
            ("patch_sha256", "patches/commit.patch"),
            ("checker_sha256", "csa/SAGenTestChecker.cpp"),
            ("metadata_sha256", "metadata/candidate.json"),
            ("source_plan_sha256", "patchweaver_plan.json"),
        ):
            package.bound(input_root / filename, row[key_name])
        case = evidence_root / case_id
        replay = load(package.root / case / "EVIDENCE_REPLAY_MANIFEST.json")
        package.bound(case / "csa/evidence_bundle.json", replay["evidence_bundle_sha256"])
        bundle = load(package.root / case / "csa/evidence_bundle.json")
        for record in bundle["records"]:
            origin = str(record["provenance"]["origin"])
            origins[origin] += 1
            if origin != "analyzer_internal":
                continue
            scope = record["scope"]
            audit_row = audit_rows[case_id]
            require(scope["file"] in audit_row["patch_files"], f"internal file scope: {case_id}")
            require(scope["function"] in audit_row["patch_functions"].get(scope["file"], []), f"internal function scope: {case_id}")
            payload = record["semantic_payload"]
            require(payload["interface"] == "clang-18:debug.DumpCFG+debug.DumpCallGraph", f"internal interface: {case_id}")
            require(payload["output_schema"] == "semweaver.csa_cfg_snapshot.v1", f"internal schema: {case_id}")
            marker = "/fse_revision/"
            raw = str(payload["raw_output_path"])
            require(marker in raw, f"raw path origin: {case_id}")
            relative = Path("evidence") / raw.split(marker, 1)[1]
            require(".." not in relative.parts, f"raw path escape: {case_id}")
            package.bound(relative, payload["raw_output_sha256"])
    require(dict(origins) == {"analyzer_internal": 80, "analyzer_output": 51, "source_derived": 61}, "origin census")
    require(audit["bound_internal_records"] == 80, "internal raw binding count")
    return ids, origins


def read_csv(package: Package, relative: str) -> list[dict[str, str]]:
    with (package.root / relative).open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def score(rows: list[dict[str, str]], prefix: str) -> dict[str, float | int]:
    tp = sum(int(row[f"{prefix}_v"]) > 0 for row in rows)
    fp = sum(int(row[f"{prefix}_f"]) > 0 for row in rows)
    fn = len(rows) - tp
    return {
        "TP": tp, "FP": fp, "FN": fn,
        "PDS": sum(int(row[f"{prefix}_pds"]) for row in rows),
        "fixed_object_alerts": sum(int(row[f"{prefix}_f"]) for row in rows),
        "F1": 2 * tp / (2 * tp + fp + fn) if (2 * tp + fp + fn) else 0.0,
    }


def verify_case_tables(package: Package, ids: list[str]) -> dict[str, dict[str, float | int]]:
    rows = read_csv(package, "results/all39_case_table_v1/CASE_TABLE.csv")
    require(len(rows) == 39 and {row["case_id"] for row in rows} == set(ids), "system CSV denominator")
    require(sum(int(row["start_f"]) for row in rows) == 71, "starting fixed alerts")
    require(sum(int(row["start_v"]) > 0 for row in rows) == 14, "starting vulnerable hits")
    summaries = {
        "kn": "all39_knighter_system_v10",
        "gpt": "e4_all39_gpt6luna_high_portfolio_v19",
        "glm53": "e4_all39_glm53_mixed_wire_portfolio_v20",
        "flash": "e4_all39_glmflash_mixed_wire_portfolio_v20",
    }
    scores: dict[str, dict[str, float | int]] = {}
    for prefix, name in summaries.items():
        observed = score(rows, prefix)
        expected = load(package.root / "results" / name / "RESULT.json")["post_adoption"]
        for key, value in observed.items():
            require(abs(value - expected[key]) < 1e-10, f"system arithmetic: {prefix}/{key}")
        scores[prefix] = observed
    require([scores[prefix]["PDS"] for prefix in summaries] == [1, 6, 10, 8], "system PDS")

    matched = read_csv(package, "results/matched14_case_table_v1/CASE_TABLE.csv")
    noisy = {row["case_id"] for row in rows if int(row["start_v"]) > 0 and int(row["start_f"]) > 0}
    require(len(matched) == 14 and {row["case_id"] for row in matched} == noisy, "matched subjects")
    for column, expected in (
        ("kn_calls", 91), ("native_calls", 82), ("no_internal_calls", 89),
        ("start_fixed", 71), ("kn_fixed", 70), ("native_fixed", 58),
        ("no_internal_fixed", 69), ("kn_pds", 1), ("native_pds", 3),
        ("no_internal_pds", 2),
    ):
        require(sum(int(row[column]) for row in matched) == expected, f"matched arithmetic: {column}")
    require(all(int(row["kn_calls"]) == int(row["call_cap"]) for row in matched), "matched per-case call caps")
    require(all(int(row["native_calls"]) <= int(row["call_cap"]) and int(row["no_internal_calls"]) <= int(row["call_cap"]) for row in matched), "matched budget overflow")

    e3 = read_csv(package, "results/e3_repeat12_case_table_v1/CASE_TABLE.csv")
    require(len(e3) == 72, "E3 repeated row denominator")
    groups: dict[tuple[str, str, str], list[dict[str, str]]] = defaultdict(list)
    for row in e3:
        groups[(row["arm"], row["stratum"], row["repeat"])].append(row)
    expected_e3 = {
        ("native", "precision"): (0, 0, 1),
        ("native", "recovery"): (2, 2, 2),
        ("no_internal", "precision"): (0, 0, 1),
        ("no_internal", "recovery"): (1, 3, 1),
    }
    subset_root = package.root / "inputs/e3_e4_repeat_subset_v10"
    subset_ids = {
        "precision": {row["case_id"] for row in load(subset_root / "PRECISION_COHORT_MANIFEST.json")["selected_rows"]},
        "recovery": {row["case_id"] for row in load(subset_root / "RECOVERY_COHORT_MANIFEST.json")["selected_rows"]},
    }
    require(len(subset_ids["precision"]) == 4 and len(subset_ids["recovery"]) == 8, "repeat subset denominator")
    require(len(groups) == 12, "E3 group count")
    for (arm, stratum), counts in expected_e3.items():
        for i, expected in enumerate(counts, 1):
            name = (arm, stratum, f"rep{i:02d}")
            group = groups[name]
            require(len(group) == (4 if stratum == "precision" else 8), f"E3 group size: {name}")
            require({row["case_id"] for row in group} == subset_ids[stratum], f"E3 subject drift: {name}")
            require(sum(int(row["pds"]) for row in group) == expected, f"E3 PDS: {name}")
    return scores


def batch_hashes(value: object) -> set[str]:
    found: set[str] = set()
    if isinstance(value, dict):
        for key, child in value.items():
            if key.endswith("batch_result_sha256") and isinstance(child, str) and len(child) == 64:
                found.add(child)
            found.update(batch_hashes(child))
    elif isinstance(value, list):
        for child in value:
            found.update(batch_hashes(child))
    return found


def verify_batches(package: Package) -> dict[str, int]:
    # scan-build can return success while the analyzer crashes or rejects its
    # command line. Never accept the validator's Boolean without its raw files.
    for path in (package.root / "evidence").rglob("RESULT.json"):
        if "frozen_validation" not in path.parts and "starting_screen" not in path.parts:
            continue
        verdict = load(path)
        if verdict.get("execution_valid") is not True:
            continue
        failures = [f for f in path.parent.rglob("*") if f.is_file() and "failures" in f.relative_to(path.parent).parts and f.name.endswith((".info.txt", ".stderr.txt")) and f.read_bytes().strip()]
        require(not failures, f"false-valid analyzer execution contains failure artifacts: {path.relative_to(package.root)}")
    batches = list((package.root / "evidence/batches").glob("*/BATCH_RESULT.json"))
    repeats = list((package.root / "evidence/repeats").glob("*/BATCH_RESULT.json"))
    subset_root = package.root / "inputs/e3_e4_repeat_subset_v10"
    subset_ids = {
        "precision_refine": {row["case_id"] for row in load(subset_root / "PRECISION_COHORT_MANIFEST.json")["selected_rows"]},
        "target_hit_recovery": {row["case_id"] for row in load(subset_root / "RECOVERY_COHORT_MANIFEST.json")["selected_rows"]},
    }
    original_hash_to_path = {package.original_sha(path.relative_to(package.root)): path for path in batches}
    expected: set[str] = set()
    for path in (package.root / "results").glob("*/RESULT.json"):
        expected.update(batch_hashes(load(path)))
    require(expected.issubset(original_hash_to_path), "portfolio batch source absent")
    accepted = 0
    for path in batches + repeats:
        root = path.parent
        result = load(path)
        require(result["completed_records"] == result["requested_cases"], f"incomplete batch: {root.name}")
        manifest = root / "BATCH_MANIFEST.json"
        frozen = load(manifest) if manifest.is_file() else None
        if manifest.is_file() and result.get("batch_manifest_sha256"):
            package.bound(manifest.relative_to(package.root), result["batch_manifest_sha256"])
        if path in repeats:
            require(frozen is not None, f"repeat manifest missing: {root.name}")
            require({row["case_id"] for row in frozen["cases"]} == subset_ids[frozen["objective"]], f"repeat batch subject drift: {root.name}")
        if result.get("rows") is not None and result.get("llm_calls") is not None:
            require(sum(row.get("llm_calls", 0) for row in result["rows"]) == result["llm_calls"], f"batch call accounting: {root.name}")
        for row in result.get("rows", []):
            if "llm_calls" in row and "call_cap" in row:
                require(row["llm_calls"] <= row["call_cap"], f"subject call overflow: {root.name}/{row['case_id']}")
                require(sum(item["llm_calls"] for item in row.get("attempt_rows", [])) == row["llm_calls"], f"attempt call accounting: {root.name}/{row['case_id']}")
                if frozen is not None:
                    require(all(item["llm_calls"] <= frozen["attempt_cap"] for item in row.get("attempt_rows", [])), f"attempt call overflow: {root.name}/{row['case_id']}")
            if row.get("accepted_pds") is not True:
                continue
            selected = row.get("selected_attempt")
            require(isinstance(selected, int) and selected > 0, f"selected attempt: {root.name}/{row['case_id']}")
            attempt = next((item for item in row["attempt_rows"] if item["attempt"] == selected), None)
            require(attempt is not None, f"attempt row missing: {root.name}/{row['case_id']}")
            case = root / str(row["case_id"]) / f"attempt-{selected:02d}"
            candidate = case / "SAGenTestChecker.cpp"
            validation = case / "frozen_validation/RESULT.json"
            package.bound(candidate.relative_to(package.root), attempt["candidate_sha256"])
            package.bound(validation.relative_to(package.root), attempt["validation_result_sha256"])
            verdict = load(validation)
            require(verdict["execution_valid"] and verdict["pds"], f"adopted candidate invalid: {root.name}/{row['case_id']}")
            require(verdict["vulnerable_alerts"] > 0 and verdict["fixed_alerts"] == 0, f"adopted pair counts: {root.name}/{row['case_id']}")
            accepted += 1
    require(len(repeats) == package.manifest["model_repeat_batch_count"] == 24, "repeat batch count")
    require(len(expected) == package.manifest["bound_portfolio_batch_count"], "bound batch count")
    return {"bound_portfolio_batches": len(expected), "repeat_batches": len(repeats), "validated_adoptions": accepted}


def verify_e2(package: Package) -> dict[str, int]:
    inputs = load(package.root / "evidence/e2_codeql/auto_loop_416_v10/INPUT_MANIFEST.json")
    package.bound("evidence/e2_codeql/initial_generated_query.ql", inputs["initial_query_sha256"])
    package.bound("evidence/e2_codeql/fix.patch", inputs["patch_sha256"])
    package.bound("evidence/e2_codeql/416_vulnerable_database.yml", inputs["vulnerable_db_manifest_sha256"])
    package.bound("evidence/e2_codeql/416_fixed_database.yml", inputs["fixed_db_manifest_sha256"])
    summary = load(package.root / "evidence/e2_codeql/auto_loop_416_repeats_v10/SUMMARY.json")
    rows = summary["rows"]
    require(len(rows) == 3, "CodeQL repeat count")
    pds = sum(row["strict_pds"] is True for row in rows)
    require(pds == 2 and all(row["model_calls"] > 0 for row in rows), "CodeQL automatic outcomes")
    return {"decodes": 3, "patch_local_pds": pds}


def verify_motivating_example(package: Package) -> dict[str, int]:
    prefix = Path("evidence/motivating_example")
    selection = load(package.root / prefix / "FINAL_SELECTION.json")
    digest = selection["candidate_sha256"]
    package.bound(prefix / "runs/quality_r4/SAGenTestChecker.cpp", digest)
    package.bound(prefix / "baseline/RESULT.json", selection["paired_before_result_sha256"])
    package.bound(prefix / "runs/quality_r4/frozen_validation/RESULT.json", selection["paired_after_result_sha256"])
    before = load(package.root / prefix / "baseline/RESULT.json")
    after = load(package.root / prefix / "runs/quality_r4/frozen_validation/RESULT.json")
    require(before["execution_valid"] and (before["vulnerable_alerts"], before["fixed_alerts"]) == (1, 1), "example baseline pair")
    require(after["execution_valid"] and after["pds"] and after["candidate_sha256"] == digest, "example final valid checker")
    require((after["vulnerable_alerts"], after["fixed_alerts"]) == (1, 0), "example final pair")
    unsafe = safe = calls = 0
    for name, row in selection["suites"].items():
        relative = prefix / "diagnostics/results" / f"quality_r4_{name}/RESULT.json"
        package.bound(relative, row["result_sha256"])
        result = load(package.root / relative)
        require(result["checker_sha256"] == digest and result["robust"], f"example checker/suite: {name}")
        require(all(item["passed"] and item["return_code"] == 0 for item in result["results"]), f"example suite failures: {name}")
        for key in ("positive_passed", "positive_total", "negative_passed", "negative_total"):
            require(result[key] == row[key], f"example suite count: {name}/{key}")
        unsafe += result["positive_passed"]
        safe += result["negative_passed"]
    require((unsafe, safe) == (58, 56), "example diagnostic denominator")
    oracle_count = 0
    for name in ("runtime_oracle", "transfer_runtime_oracle"):
        result = load(package.root / prefix / "diagnostics" / name / "RESULT.json")
        require(result["all_passed"] and result["cases"] == result["passed"], "example runtime labels")
        oracle_count += result["cases"]
    require(oracle_count == 80, "example runtime label denominator")
    previous = package.original_sha(prefix / "baseline/SAGenTestChecker.cpp")
    for index, row in enumerate(selection["model_lineage"]):
        root = prefix / "runs" / ("initial_flash" if index == 0 else f"quality_r{index}")
        package.bound(root / "SAGenTestChecker.cpp", row["candidate_sha256"])
        package.bound(root / "RUN_MANIFEST.json", row["manifest_sha256"])
        package.bound(root / "llm_exchanges.jsonl", row["exchange_sha256"])
        manifest = load(package.root / root / "RUN_MANIFEST.json")
        require(manifest["starting_checker_sha256"] == previous, "example edit lineage")
        require(manifest["model"] == row["model"] and manifest["model_calls_used"] == row["model_calls"], "example model accounting")
        previous = row["candidate_sha256"]
        calls += row["model_calls"]
    require(previous == digest and calls == selection["total_model_calls"] == 14, "example final lineage/accounting")
    return {"unsafe_passed": unsafe, "safe_passed": safe, "runtime_labels": oracle_count, "model_calls": calls}


def verify_backend_cost(package: Package) -> dict[str, int]:
    base = Path("evidence/backend_cost")
    probe = base / "backend_cost_20260927_v1"
    result = load(package.root / probe / "RESULT.json")
    package.bound(probe / "INPUT_MANIFEST.json", result["input_manifest_sha256"])
    manifest = load(package.root / probe / "INPUT_MANIFEST.json")
    package.bound(base / "scripts/measure_backend_cost.py", manifest["driver_sha256"])
    require(load(package.root / probe / "CODEQL_VERSION.json")["version"] == "2.26.4", "CodeQL cost CLI version")
    require(result["completed"] == len(result["rows"]) == 12, "cost replay denominator")
    require(result["all_execution_valid"] and result["all_outcomes_match"], "cost valid observations")
    cells = set()
    for row in result["rows"]:
        cell = row["repeat"], row["variant"], row["side"]
        require(cell not in cells, "duplicate cost replay")
        cells.add(cell)
        require(row["execution_valid"] and row["outcome_matches"], "invalid cost replay")
        require(row["query"]["exit_code"] == row["decode"]["exit_code"] == 0, "cost process failure")
        expected = 8 if row["variant"] == "refined" and row["side"] == "vulnerable" else 0
        require(row["rows"] == row["expected_rows"] == expected, "cost query outcome drift")
        directory = probe / f"rep-{row['repeat']:02d}-{row['variant']}"
        queries = list((package.root / directory).glob("*.ql"))
        require(len(queries) == 1, "cost query missing")
        package.bound(queries[0].relative_to(package.root), manifest["query_hashes"][row["variant"]])
        package.bound(directory / "qlpack.yml", manifest["pack_hashes"][row["variant"]])
        decoded = load(package.root / directory / f"{row['side']}-decode.stdout.log")
        require(len(decoded["#select"]["tuples"]) == expected, "cost raw tuple count")
    require(cells == {(r, v, s) for r in (1, 2, 3) for v in ("initial", "refined") for s in ("vulnerable", "fixed")}, "cost replay design")
    for variant in ("initial", "refined"):
        times = [sum(row["query"]["wall_seconds"] + row["decode"]["wall_seconds"] for row in result["rows"] if row["variant"] == variant and row["repeat"] == r) for r in (1, 2, 3)]
        require(times == result["summary"][variant]["paired_seconds"], "cost pair timing sum")
        require(statistics.median(times) == result["summary"][variant]["median_paired_seconds"], "cost median")
    joined_root = base / "backend_cost_summary_20260927_v1"
    joined = load(package.root / joined_root / "RESULT.json")
    package.bound(probe / "RESULT.json", joined["replay_result_sha256"])
    for row in joined["database_setup"]:
        package.bound(joined_root / row["log"], row["log_sha256"])
    for row in joined["retained_model_runs"]:
        e2 = Path("evidence/e2_codeql") / row["run"]
        package.bound(e2 / "RUN_RESULT.json", row["result_sha256"])
        for run in row["round_manifests"]:
            relative = Path(run["path"]).parts[1:]
            package.bound(Path("evidence/e2_codeql").joinpath(*relative), run["sha256"])
        require(sum(r["model_calls"] for r in row["round_manifests"]) == row["model_calls"], "cost model accounting")
        require(sum(r["total_tokens"] for r in row["round_manifests"]) == row["total_tokens"], "cost token accounting")
    inventory = load(package.root / base / "backend_integration_audit_20260927_v1/RESULT.json")
    require(inventory["historical_developer_hours"] is None, "unsupported labor estimate")
    for row in inventory["rows"]:
        prefix = base / "scripts" if row["category"] == "external_e2_protocol_and_tests" else Path("source/SemWeaver")
        package.bound(prefix / row["path"], row["sha256"])
    return {"side_replays": len(cells), "model_decodes_accounted": len(joined["retained_model_runs"])}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    args = parser.parse_args()
    package = Package(args.package)
    verify_private_data(package)
    ids, origins = verify_inputs_and_evidence(package)
    scores = verify_case_tables(package, ids)
    batches = verify_batches(package)
    e2 = verify_e2(package)
    example = verify_motivating_example(package)
    cost = verify_backend_cost(package)
    print(json.dumps({
        "status": "verified_offline",
        "cases": len(ids), "evidence_origins": dict(origins),
        "portfolio_PDS": {key: value["PDS"] for key, value in scores.items()},
        **batches, "codeql": e2, "motivating_example": example, "backend_cost": cost,
    }, indent=2))


if __name__ == "__main__":
    main()
