#!/usr/bin/env python3
"""No-network integrity and outcome verifier for the SemWeaver V7 review package."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from collections import Counter
from pathlib import Path


def sha(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


class Evidence:
    def __init__(self, root: Path) -> None:
        self.root = root
        manifest = load(root / "ARTIFACT_MANIFEST.json")
        require(manifest["file_count"] == len(manifest["files"]), "manifest file count")
        expected = set()
        for row in manifest["files"]:
            relative = str(row["path"])
            require(relative not in expected, f"duplicate manifest path: {relative}")
            expected.add(relative)
            path = root / relative
            require(path.is_file(), f"missing file: {relative}")
            require(path.stat().st_size == row["size"], f"size mismatch: {relative}")
            require(sha(path) == row["sha256"], f"hash mismatch: {relative}")
        actual = {
            path.relative_to(root).as_posix() for path in root.rglob("*")
            if path.is_file() and path.name != "ARTIFACT_MANIFEST.json"
        }
        require(actual == expected, f"manifest coverage mismatch: {len(actual ^ expected)} paths")
        redactions = load(root / "REDACTIONS.json")
        require(redactions["file_count"] == len(redactions["files"]), "redaction count")
        self.redactions = {row["path"]: row for row in redactions["files"]}
        require(len(self.redactions) == len(redactions["files"]), "duplicate redaction path")
        for relative, row in self.redactions.items():
            require(relative in expected, f"redaction target absent: {relative}")
            require(sha(root / relative) == row["redacted_sha256"], f"redaction output drift: {relative}")

    def original_sha(self, relative: str | Path) -> str:
        relative = Path(relative).as_posix()
        path = self.root / relative
        require(path.is_file(), f"bound file missing: {relative}")
        row = self.redactions.get(relative)
        return row["original_sha256"] if row else sha(path)

    def bound(self, relative: str | Path, expected: str) -> None:
        require(self.original_sha(relative) == expected, f"original digest mismatch: {relative}")


def verify_inputs(evidence: Evidence) -> list[str]:
    base = Path("evidence/semweaver_treatment_evidence_cohort_v3")
    batch = load(evidence.root / base / "BATCH_MANIFEST.json")
    cases = batch["cases"]
    require(len(cases) == 12, "case denominator")
    ids = [str(row["case_id"]) for row in cases]
    require(len(set(ids)) == 12, "case uniqueness")
    for row in cases:
        case_id = str(row["case_id"])
        prefix = Path("inputs/e1/cases") / case_id
        for key, filename in (
            ("patch_sha256", "patches/commit.patch"),
            ("checker_sha256", "csa/SAGenTestChecker.cpp"),
            ("metadata_sha256", "metadata/candidate.json"),
            ("source_plan_sha256", "patchweaver_plan.json"),
        ):
            evidence.bound(prefix / filename, row[key])
    blobs = load(evidence.root / "inputs/VULNERABLE_SOURCE_BLOBS.json")
    require(blobs["blob_count"] == len(blobs["blobs"]), "Linux source blob count")
    for row in blobs["blobs"]:
        relative = Path("inputs/vulnerable_source") / row["case_id"] / row["path"]
        data = (evidence.root / relative).read_bytes()
        require(len(data) == row["size"], f"Linux blob size: {relative}")
        require(hashlib.sha256(data).hexdigest() == row["sha256"], f"Linux blob SHA: {relative}")
        git_blob = hashlib.sha1(f"blob {len(data)}\0".encode() + data).hexdigest()
        require(git_blob == row["git_blob_sha1"], f"Linux Git blob: {relative}")
    return ids


def verify_internal_evidence(evidence: Evidence, ids: list[str]) -> dict:
    base = Path("evidence/semweaver_treatment_evidence_cohort_v3")
    audit = load(evidence.root / base / "BINDING_AUDIT.json")
    audit_rows = {row["case_id"]: row for row in audit["rows"]}
    counts: Counter[str] = Counter()
    for case_id in ids:
        bundle_path = base / case_id / "csa/evidence_bundle.json"
        replay_path = base / case_id / "EVIDENCE_REPLAY_MANIFEST.json"
        replay = load(evidence.root / replay_path)
        evidence.bound(bundle_path, replay["evidence_bundle_sha256"])
        bundle = load(evidence.root / bundle_path)
        for record in bundle["records"]:
            origin = str(record["provenance"]["origin"])
            counts[origin] += 1
            if origin != "analyzer_internal":
                continue
            scope = record["scope"]
            require(scope["file"] in audit_rows[case_id]["patch_files"], f"internal file scope: {case_id}")
            require(scope["function"] in audit_rows[case_id]["patch_functions"].get(scope["file"], []), f"internal function scope: {case_id}")
            payload = record["semantic_payload"]
            require(payload["interface"] == "clang-18:debug.DumpCFG+debug.DumpCallGraph", f"internal interface: {case_id}")
            require(payload["output_schema"] == "semweaver.csa_cfg_snapshot.v1", f"internal schema: {case_id}")
            raw = str(payload["raw_output_path"])
            require("/fse_revision/" in raw, f"internal raw path: {case_id}")
            relative = Path("evidence") / raw.split("/fse_revision/", 1)[1]
            evidence.bound(relative, payload["raw_output_sha256"])
    require(dict(counts) == {"analyzer_internal": 37, "analyzer_output": 18, "source_derived": 15}, "record origin counts")
    require(sum(counts.values()) == 70 and audit["bound_internal_records"] == 37, "record denominator")
    return dict(counts)


def verify_repeats(evidence: Evidence, ids: list[str]) -> dict:
    base = Path("evidence/repeats_v7")
    aggregate = load(evidence.root / base / "V7_REPEAT_SUMMARY.json")
    require(aggregate["distinct_subjects"] == 12 and aggregate["replicate_count"] == 3, "repeat denominator")
    results = []
    for name in ("rep01", "rep02", "rep03"):
        rep = base / name
        strict_path = rep / "knighter_strict_pds/STRICT_RESULT.json"
        strict = load(evidence.root / strict_path)
        require(strict["subjects"] == 12 and strict["unscored"] == 0, f"strict completeness: {name}")
        evidence.bound(strict_path, aggregate["input_sha256"][name]["strict"])
        baseline_rows = {row["case_id"]: row for row in strict["rows"]}
        require(set(baseline_rows) == set(ids), f"baseline subjects: {name}")
        sets = {"knighter": {key for key, row in baseline_rows.items() if row["strict_pds"] is True}}
        require(len(sets["knighter"]) == strict["strict_pds"], f"baseline PDS: {name}")
        for case_id in sets["knighter"]:
            row = baseline_rows[case_id]
            result_path = rep / "knighter_strict_pds" / case_id / "RESULT.json"
            evidence.bound(result_path, row["result_sha256"])
            validation = load(evidence.root / result_path)
            require(validation["execution_valid"] and validation["pds"], f"baseline validation: {name}/{case_id}")
            require(validation["vulnerable_alerts"] > 0 and validation["fixed_alerts"] == 0, f"baseline paired counts: {name}/{case_id}")
        for arm in ("native", "no_internal"):
            manifest_path = rep / arm / "BATCH_MANIFEST.json"
            result_path = rep / arm / "BATCH_RESULT.json"
            evidence.bound(manifest_path, aggregate["input_sha256"][name][f"{arm}_manifest"])
            evidence.bound(result_path, aggregate["input_sha256"][name][f"{arm}_result"])
            manifest, result = load(evidence.root / manifest_path), load(evidence.root / result_path)
            require(result["requested_cases"] == result["completed_records"] == 12, f"arm complete: {name}/{arm}")
            require(result["batch_manifest_sha256"] == evidence.original_sha(manifest_path), f"arm manifest binding: {name}/{arm}")
            require(manifest["total_model_call_cap"] == strict["baseline_model_calls"], f"matched cap: {name}/{arm}")
            require(result["llm_calls"] <= manifest["total_model_call_cap"], f"call overflow: {name}/{arm}")
            arm_rows = {row["case_id"]: row for row in result["rows"]}
            require(set(arm_rows) == set(ids), f"arm subjects: {name}/{arm}")
            sets[arm] = {key for key, row in arm_rows.items() if row["accepted_pds"] is True}
            require(len(sets[arm]) == result["accepted_pds"], f"arm PDS count: {name}/{arm}")
            for case_id in sets[arm]:
                selected = arm_rows[case_id]["selected_attempt"]
                require(isinstance(selected, int) and selected > 0, f"selected attempt: {name}/{arm}/{case_id}")
                validation_path = rep / arm / case_id / f"attempt-{selected:02d}" / "frozen_validation/RESULT.json"
                validation = load(evidence.root / validation_path)
                require(validation["execution_valid"] and validation["pds"], f"arm validation: {name}/{arm}/{case_id}")
                require(validation["vulnerable_alerts"] > 0 and validation["fixed_alerts"] == 0, f"arm paired counts: {name}/{arm}/{case_id}")
                attempt = next(row for row in arm_rows[case_id]["attempt_rows"] if row["attempt"] == selected)
                evidence.bound(validation_path, attempt["validation_result_sha256"])
                evidence.bound(rep / arm / case_id / f"attempt-{selected:02d}" / "SAGenTestChecker.cpp", attempt["candidate_sha256"])
        row = next(item for item in aggregate["replicates"] if item["replicate"] == name)
        for arm in ("knighter", "native", "no_internal"):
            require(row["behavior_pds"][arm] == len(sets[arm]), f"aggregate PDS: {name}/{arm}")
        results.append({"replicate": name, "knighter": len(sets["knighter"]), "native": len(sets["native"]), "no_internal": len(sets["no_internal"])})
    return {"replicates": results}


def verify_expansion(evidence: Evidence) -> dict:
    path = evidence.root / "evidence/expansion_validation_v2/VALIDATION_RESULT.json"
    result = load(path)
    require(result["requested_cases"] == result["completed_records"] == 10, "expansion completeness")
    require(len({row["case_id"] for row in result["rows"]}) == 10, "expansion uniqueness")
    return dict(Counter(row["status"] for row in result["rows"]))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    args = parser.parse_args()
    evidence = Evidence(args.package.resolve())
    ids = verify_inputs(evidence)
    origins = verify_internal_evidence(evidence, ids)
    repeats = verify_repeats(evidence, ids)
    expansion = verify_expansion(evidence)
    print(json.dumps({
        "status": "verified_offline", "cases": len(ids), "origins": origins,
        **repeats, "expansion_statuses": expansion,
    }, indent=2))


if __name__ == "__main__":
    main()
