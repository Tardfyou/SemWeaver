#!/usr/bin/env python3
"""Verify packaged hashes and the paper's completed outcome invariants."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
from collections import Counter
from pathlib import Path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def require(value: bool, message: str) -> None:
    if not value:
        raise AssertionError(message)


def original_sha(root: Path, redactions: dict[str, dict], relative: str) -> str:
    path = root / relative
    require(path.is_file(), f"missing bound artifact: {relative}")
    entry = redactions.get(relative)
    if entry is None:
        return sha256(path)
    require(sha256(path) == entry["redacted_sha256"], f"redaction hash mismatch: {relative}")
    return str(entry["original_sha256"])


def require_digest(root: Path, redactions: dict[str, dict], relative: str, expected: str) -> None:
    require(original_sha(root, redactions, relative) == expected, f"original hash mismatch: {relative}")


def verify_frozen_inputs(root: Path, redactions: dict[str, dict]) -> list[str]:
    cohort = load(root / "evidence/semweaver_treatment_evidence_cohort_v3/BATCH_MANIFEST.json")
    cases = cohort["cases"]
    require(len(cases) == cohort["case_count"] == 12, "frozen case denominator drift")
    seen: set[str] = set()
    for item in cases:
        case_id = str(item["case_id"])
        require(case_id not in seen, f"duplicate case: {case_id}")
        seen.add(case_id)
        prefix = f"inputs/e1/cases/{case_id}"
        for field, relative in (
            ("source_plan_sha256", "patchweaver_plan.json"),
            ("patch_sha256", "patches/commit.patch"),
            ("checker_sha256", "csa/SAGenTestChecker.cpp"),
            ("metadata_sha256", "metadata/candidate.json"),
        ):
            require_digest(root, redactions, f"{prefix}/{relative}", item[field])
    source_index = load(root / "inputs/e1/VULNERABLE_SOURCE_BLOBS.json")
    require(source_index["blob_count"] == len(source_index["blobs"]), "source blob count drift")
    source_keys: set[tuple[str, str]] = set()
    for item in source_index["blobs"]:
        case_id, file_name = str(item["case_id"]), str(item["path"])
        key = (case_id, file_name)
        require(case_id in seen and key not in source_keys, f"unexpected source blob: {key}")
        source_keys.add(key)
        require(item["revision"] == f"{item['commit_id']}^", f"source revision mismatch: {key}")
        case_meta = load(root / f"inputs/e1/cases/{case_id}/metadata/candidate.json")
        require(item["commit_id"] == case_meta["commit_id"], f"source commit mismatch: {key}")
        path = root / "inputs/e1/vulnerable_source" / case_id / file_name
        data = path.read_bytes()
        require(len(data) == item["size"], f"source blob size mismatch: {key}")
        require(hashlib.sha256(data).hexdigest() == item["sha256"], f"source blob SHA-256 mismatch: {key}")
        git_blob = hashlib.sha1(f"blob {len(data)}\0".encode("ascii") + data).hexdigest()
        require(git_blob == item["git_blob_sha1"], f"source Git blob mismatch: {key}")
    return sorted(seen)


def verify_evidence(root: Path, redactions: dict[str, dict], case_ids: list[str]) -> tuple[dict[str, int], int, int]:
    prefix = "evidence/semweaver_treatment_evidence_cohort_v3"
    batch = load(root / f"{prefix}/BATCH_RESULT.json")
    require(set(row["case_id"] for row in batch["rows"]) == set(case_ids), "evidence case mismatch")
    require_digest(root, redactions, f"{prefix}/BATCH_MANIFEST.json", batch["batch_manifest_sha256"])
    audit = load(root / f"{prefix}/BINDING_AUDIT.json")
    audit_rows = {item["case_id"]: item for item in audit["rows"]}
    source_index = load(root / "inputs/e1/VULNERABLE_SOURCE_BLOBS.json")
    source_keys = {(item["case_id"], item["path"]) for item in source_index["blobs"]}
    expected_source_keys = {
        (row["case_id"], file_name)
        for row in audit["rows"]
        for file_name in row["patch_files"]
    }
    require(source_keys == expected_source_keys, "source blob coverage drift")
    counts: Counter[str] = Counter()
    native = 0
    excerpt_verified = 0
    excerpt_missing = 0
    for row in batch["rows"]:
        case_id = str(row["case_id"])
        case_prefix = f"{prefix}/{case_id}"
        replay_relative = f"{case_prefix}/EVIDENCE_REPLAY_MANIFEST.json"
        require_digest(root, redactions, replay_relative, row["result_sha256"])
        replay = load(root / replay_relative)
        bundle_relative = f"{case_prefix}/csa/evidence_bundle.json"
        require_digest(root, redactions, bundle_relative, replay["evidence_bundle_sha256"])
        bundle = load(root / bundle_relative)
        internal_for_case = 0
        for record in bundle["records"]:
            origin = str(record.get("provenance", {}).get("origin", "unknown"))
            counts[origin] += 1
            if origin != "analyzer_internal":
                continue
            native += 1
            internal_for_case += 1
            scope = record["scope"]
            audit_row = audit_rows[case_id]
            require(scope["file"] in audit_row["patch_files"], f"native file scope mismatch: {case_id}")
            require(
                scope["function"] in audit_row["patch_functions"].get(scope["file"], []),
                f"native function scope mismatch: {case_id}",
            )
            payload = record["semantic_payload"]
            excerpt = str(payload.get("source_excerpt", ""))
            if excerpt:
                lines = (root / "inputs/e1/vulnerable_source" / case_id / scope["file"]).read_text(
                    encoding="utf-8"
                ).splitlines()
                numbered = [
                    (int(match.group(1)), match.group(2))
                    for line in excerpt.splitlines()
                    if (match := re.match(r"^(\d+): (.*)$", line))
                ]
                require(bool(numbered), f"unparseable source excerpt: {case_id}")
                require(
                    all(0 < number <= len(lines) and lines[number - 1] == content for number, content in numbered),
                    f"source excerpt revision mismatch: {case_id}",
                )
                excerpt_verified += 1
            else:
                excerpt_missing += 1
            require(payload["interface"] == "clang-18:debug.DumpCFG+debug.DumpCallGraph", f"native interface mismatch: {case_id}")
            require(payload["output_schema"] == "semweaver.csa_cfg_snapshot.v1", f"native schema mismatch: {case_id}")
            require(bool(payload.get("command_sha256")), f"missing command hash: {case_id}")
            raw_path = str(payload["raw_output_path"])
            require("/fse_revision/" in raw_path, f"unexpected native raw path: {case_id}")
            raw_relative = "evidence/" + raw_path.split("/fse_revision/", 1)[1]
            require_digest(root, redactions, raw_relative, payload["raw_output_sha256"])
        require(internal_for_case == audit_rows[case_id]["internal_records"], f"native count mismatch: {case_id}")
        require(internal_for_case == row["origin_counts"].get("analyzer_internal", 0), f"evidence row mismatch: {case_id}")
    provenance = load(root / f"{prefix}/PROVENANCE_AUDIT.json")["summary"]
    require(dict(counts) == {k: v for k, v in provenance["origin_counts"].items() if v}, "origin recount mismatch")
    require(sum(counts.values()) == provenance["total_records"] == 70, "record denominator drift")
    require(native == audit["bound_internal_records"] == 37, "native binding count drift")
    require(excerpt_verified + excerpt_missing == native, "excerpt accounting mismatch")
    return dict(counts), excerpt_verified, excerpt_missing


def verify_outcomes(root: Path, redactions: dict[str, dict], case_ids: list[str]) -> dict:
    observed = root / "source/SemWeaver/experiments/knighter/experiment"
    k_summary = load(observed / "observed_matched_knighter_cohort.json")
    m_summary = load(observed / "observed_semweaver_matched_metamorphic_v3.json")
    f_summary = load(observed / "observed_semweaver_full_budget_v3.json")
    burden = load(observed / "observed_deploy_or_fallback_alert_burden.json")

    k_prefix = "evidence/matched_knighter_gpt56terra_cohort_v2"
    k_batch = load(root / f"{k_prefix}/BATCH_RESULT.json")
    require(set(item["case_id"] for item in k_batch["cases"]) == set(case_ids), "Knighter case mismatch")
    k_accepted = []
    for item in k_batch["cases"]:
        result = item["result"]
        require(result["execution_valid"] is True, f"Knighter execution invalid: {item['case_id']}")
        if any(attempt["result"] == "Refined" for attempt in result["results"]):
            log_relative = f"{k_prefix}/{result['checker_id']}/MATCHED_BASELINE_RUN.log"
            log = (root / log_relative).read_text(encoding="utf-8")
            require(re.search(r"Buggy:\s+[1-9]\d* bugs found", log) is not None, f"Knighter vulnerable hit unverified: {item['case_id']}")
            require("Non-buggy: No bugs found!" in log, f"Knighter fixed silence unverified: {item['case_id']}")
            require(re.search(r"Non-buggy:\s+[1-9]\d* bugs found", log) is None, f"Knighter fixed noise remains: {item['case_id']}")
            k_accepted.append(item["case_id"])
    require(k_accepted == [k_summary["accepted_candidate"]["case_id"]], "Knighter PDS summary drift")
    for field, name in (
        ("batch_manifest", "BATCH_MANIFEST.json"),
        ("batch_result", "BATCH_RESULT.json"),
        ("independent_summary", "INDEPENDENT_SUMMARY.json"),
    ):
        require_digest(root, redactions, f"{k_prefix}/{name}", k_summary["artifact_sha256"][field])
    k_suite = "evidence/case05_matched_knighter_metamorphic"
    for field, name in (
        ("artifact_review", "ARTIFACT_REVIEW.json"),
        ("metamorphic_manifest", "FROZEN_MANIFEST.json"),
        ("metamorphic_result", "RESULT.json"),
    ):
        require_digest(root, redactions, f"{k_suite}/{name}", k_summary["artifact_sha256"][field])

    m_prefix = "evidence/semweaver_treatment_matched_v3"
    m_batch = load(root / f"{m_prefix}/BATCH_RESULT.json")
    require(set(item["case_id"] for item in m_batch["rows"]) == set(case_ids), "matched SemWeaver case mismatch")
    require_digest(root, redactions, f"{m_prefix}/BATCH_MANIFEST.json", m_batch["batch_manifest_sha256"])
    m_accepted = []
    for row in m_batch["rows"]:
        case_id = row["case_id"]
        case_prefix = f"{m_prefix}/{case_id}"
        require_digest(root, redactions, f"{case_prefix}/RUN_MANIFEST.json", row["treatment_manifest_sha256"])
        if row["validation_result_sha256"]:
            relative = f"{case_prefix}/frozen_validation/RESULT.json"
            require_digest(root, redactions, relative, row["validation_result_sha256"])
            validation = load(root / relative)
            require(bool(validation["pds"]) == bool(row["accepted_pds"]), f"matched PDS mismatch: {case_id}")
            require(validation["vulnerable_alerts"] > 0 if row["accepted_pds"] else True, f"lost vulnerable target: {case_id}")
            require(validation["fixed_alerts"] == 0 if row["accepted_pds"] else True, f"fixed noise remains: {case_id}")
        if row["accepted_pds"]:
            m_accepted.append(case_id)
    require(m_accepted == [m_summary["case_id"]], "matched SemWeaver summary drift")
    m_case = f"{m_prefix}/{m_summary['case_id']}"
    require_digest(root, redactions, f"{m_case}/SAGenTestChecker.cpp", m_summary["candidate_sha256"])
    for key, directory in (("development_suite", "metamorphic_development"), ("fresh_suite", "metamorphic_fresh")):
        relative = f"{m_case}/{directory}/RESULT.json"
        require_digest(root, redactions, relative, m_summary[key]["result_sha256"])
        outcome = load(root / relative)
        require(outcome["checker_sha256"] == m_summary["candidate_sha256"], f"matched suite candidate mismatch: {key}")
        require(outcome["robust"] is False, f"matched suite outcome drift: {key}")

    f_prefix = "evidence/semweaver_treatment_full_v3"
    f_batch = load(root / f"{f_prefix}/BATCH_RESULT.json")
    require(set(item["case_id"] for item in f_batch["rows"]) == set(case_ids), "full-budget case mismatch")
    require_digest(root, redactions, f"{f_prefix}/BATCH_MANIFEST.json", f_batch["batch_manifest_sha256"])
    f_accepted = []
    for row in f_batch["rows"]:
        case_id = row["case_id"]
        candidate_pds = False
        for attempt in row["attempt_rows"]:
            prefix = f"{f_prefix}/{case_id}/attempt-{attempt['attempt']:02d}"
            require_digest(root, redactions, f"{prefix}/RUN_MANIFEST.json", attempt["treatment_manifest_sha256"])
            require_digest(root, redactions, f"{prefix}/SAGenTestChecker.cpp", attempt["candidate_sha256"])
            if attempt["validation_result_sha256"]:
                relative = f"{prefix}/frozen_validation/RESULT.json"
                require_digest(root, redactions, relative, attempt["validation_result_sha256"])
                validation = load(root / relative)
                if validation["pds"]:
                    require(validation["vulnerable_alerts"] > 0 and validation["fixed_alerts"] == 0, f"invalid PDS: {case_id}")
                    candidate_pds = True
        require(candidate_pds == bool(row["accepted_pds"]), f"full-budget PDS mismatch: {case_id}")
        if candidate_pds:
            f_accepted.append(case_id)
    require(f_accepted == f_summary["pds"]["case_ids"], "full-budget PDS summary drift")
    for item in f_summary["metamorphic_results"]:
        case_id = item["case_id"]
        candidates = sorted((root / f"{f_prefix}/{case_id}").glob("attempt-*/metamorphic*/RESULT.json"))
        matching = [path for path in candidates if original_sha(root, redactions, path.relative_to(root).as_posix()) == item["result_sha256"]]
        require(len(matching) == 1, f"missing metamorphic result: {case_id}")
        result = load(matching[0])
        require(result["robust"] is False, f"unexpected robust result: {case_id}")
    require(f_batch["accepted_pds"] == 4 and f_batch["llm_calls"] == 86, "full-budget aggregate drift")

    counts = {
        case_id: int(load(root / f"inputs/e1/cases/{case_id}/fixed_validation.json")["diagnostics_count"])
        for case_id in case_ids
    }
    require(sum(counts.values()) == burden["starting_fixed_alerts"] == 37, "starting alert burden drift")
    for name, accepted in (
        ("knighter_model_matched", k_accepted),
        ("semweaver_call_matched", m_accepted),
        ("semweaver_full_budget", f_accepted),
    ):
        item = next(row for row in burden["conditions"] if row["condition"] == name)
        require(item["accepted_case_ids"] == accepted, f"adoption subject mismatch: {name}")
        removed = sum(counts[case_id] for case_id in accepted)
        require(item["fixed_alerts_removed"] == removed, f"removed-alert count mismatch: {name}")
        require(item["fixed_alerts_retained"] == 37 - removed, f"retained-alert count mismatch: {name}")
    return {"knighter_pds": len(k_accepted), "matched_pds": len(m_accepted), "full_pds": len(f_accepted), "fixed_retained": 37 - sum(counts[case_id] for case_id in f_accepted)}


def verify_development_sensitivities(root: Path, redactions: dict[str, dict]) -> dict[str, str]:
    """Recount post-hoc fixture audits without promoting them to effectiveness data."""
    cases = {
        "case19_matched_arity": (
            "evidence/case19_three_arg_sensitivity",
            "evidence/arity_sensitivity_matched_v3/RESULT.json",
            "evidence/semweaver_treatment_matched_v3/19_39b13dce1a91_Double_Free/SAGenTestChecker.cpp",
            "72f28f5dc9594735793a266d3aedf99db1214cbb36aba8f0e67895d663d28094",
            (0, 6, 3, 3),
        ),
        "case19_full_arity": (
            "evidence/case19_three_arg_sensitivity",
            "evidence/arity_sensitivity_full_v3/RESULT.json",
            "evidence/semweaver_treatment_full_v3/19_39b13dce1a91_Double_Free/attempt-02/SAGenTestChecker.cpp",
            "df4f51f9ea42688db369aaa8ae6f406ecc4fab9537d65b6ac5034ea0101f8032",
            (1, 6, 2, 3),
        ),
        "case19_targeted_arity": (
            "evidence/case19_three_arg_sensitivity",
            "evidence/arity_sensitivity_targeted_v2/RESULT.json",
            "evidence/semweaver_targeted_postgate_v2/19_39b13dce1a91_Double_Free/SAGenTestChecker.cpp",
            "f94b25c4a76c2cc72d1950e01d8fab151821921aa7dcb88a0f9f7fc9c0ac4e5e",
            (0, 6, 2, 3),
        ),
        "case23_bounded_consumer": (
            "evidence/case23_bounded_consumer_sensitivity",
            "evidence/case23_bounded_consumer_sensitivity/checker_replay/RESULT.json",
            "evidence/semweaver_treatment_full_v3/23_13e788deb734_Out_of_Bound/attempt-01/SAGenTestChecker.cpp",
            "a7cc2ac74f03c02394704cb16f4dcb272f887540833df9d523113e44e9917330",
            (5, 6, 2, 3),
        ),
        "case08_signed_literal": (
            "evidence/case08_signed_literal_sensitivity",
            "evidence/case08_signed_literal_sensitivity/checker_replay/RESULT.json",
            "evidence/semweaver_treatment_full_v3/08_768f17fd25e4_Integer_Overflow/attempt-02/SAGenTestChecker.cpp",
            "131fcab134b49dfe578a8b1f71b0b2e4cc354174ca56eff77c2967426421eaed",
            (2, 6, 3, 3),
        ),
        "case08_assignment_form": (
            "evidence/case08_assignment_sanity",
            "evidence/case08_assignment_sanity/checker_replay/RESULT.json",
            "evidence/semweaver_treatment_full_v3/08_768f17fd25e4_Integer_Overflow/attempt-02/SAGenTestChecker.cpp",
            "a007fd1c4a3ead690fb63f0b7a904cedf812462ac697110eb479d408abf3cf3c",
            (1, 2, 2, 2),
        ),
    }
    observed = {}
    for name, (suite_prefix, result_relative, candidate_relative, result_sha, counts) in cases.items():
        require_digest(root, redactions, result_relative, result_sha)
        result = load(root / result_relative)
        require_digest(root, redactions, candidate_relative, result["checker_sha256"])
        manifest_relative = f"{suite_prefix}/manifest.csv"
        require_digest(root, redactions, manifest_relative, result["manifest_sha256"])
        with (root / manifest_relative).open(newline="", encoding="utf-8") as handle:
            manifest_rows = {row["variant_id"]: row for row in csv.DictReader(handle)}
        fixture_rows = result["results"]
        require(len(manifest_rows) == len(fixture_rows), f"variant denominator drift: {name}")
        positive_total = negative_total = positive_passed = negative_passed = 0
        for row in fixture_rows:
            variant_id = row["variant_id"]
            require(variant_id in manifest_rows, f"unknown sensitivity variant: {name}/{variant_id}")
            expected = manifest_rows[variant_id]
            require(row["variant_kind"] == expected["variant_kind"], f"variant kind drift: {name}/{variant_id}")
            require(str(row["expected_alert"]) == str(expected["expected_alert"]), f"variant label drift: {name}/{variant_id}")
            source = Path(str(expected["source_path"]))
            require(not source.is_absolute() and ".." not in source.parts, f"unsafe fixture path: {name}/{variant_id}")
            require(str(source) == row["source_path"], f"fixture path drift: {name}/{variant_id}")
            require_digest(root, redactions, f"{suite_prefix}/{source.as_posix()}", row["source_sha256"])
            require(result["fixtures"][variant_id] == row["source_sha256"], f"fixture digest drift: {name}/{variant_id}")
            require(row["return_code"] == 0, f"fixture execution failed: {name}/{variant_id}")
            passed = bool(row["alert_count"] > 0) == (str(row["expected_alert"]) == "1")
            require(bool(row["passed"]) == passed, f"fixture outcome drift: {name}/{variant_id}")
            if row["variant_kind"] == "positive":
                positive_total += 1
                positive_passed += int(passed)
            else:
                negative_total += 1
                negative_passed += int(passed)
        recount = (positive_passed, positive_total, negative_passed, negative_total)
        require(recount == counts, f"sensitivity aggregate drift: {name}")
        require(recount == (
            result["positive_passed"], result["positive_total"],
            result["negative_passed"], result["negative_total"],
        ), f"result aggregate mismatch: {name}")
        require(result["robust"] is False, f"unexpected robust sensitivity result: {name}")
        observed[name] = f"{positive_passed}/{positive_total} positive; {negative_passed}/{negative_total} negative"
    return observed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifact_root", nargs="?", type=Path, default=Path("."))
    root = parser.parse_args().artifact_root.expanduser().resolve()
    manifest = load(root / "ARTIFACT_MANIFEST.json")
    listed: set[str] = set()
    for item in manifest["files"]:
        relative = str(item["path"])
        require(not Path(relative).is_absolute() and ".." not in Path(relative).parts, f"unsafe manifest path: {relative}")
        require(relative not in listed, f"duplicate manifest path: {relative}")
        listed.add(relative)
        path = root / relative
        require(path.is_file(), f"missing: {item['path']}")
        require(path.stat().st_size == item["size"], f"size mismatch: {item['path']}")
        require(sha256(path) == item["sha256"], f"hash mismatch: {item['path']}")
    actual = {
        path.relative_to(root).as_posix()
        for path in root.rglob("*")
        if path.is_file() and path.name != "ARTIFACT_MANIFEST.json"
    }
    require(listed == actual and len(listed) == manifest["file_count"], "artifact file inventory drift")
    redaction_data = load(root / "REDACTIONS.json")
    require(redaction_data["file_count"] == len(redaction_data["files"]), "redaction count drift")
    redactions = {str(item["path"]): item for item in redaction_data["files"]}
    require(len(redactions) == redaction_data["file_count"], "duplicate redaction path")
    require(set(redactions).issubset(listed), "redaction refers to missing file")
    for relative in redactions:
        original_sha(root, redactions, relative)

    case_ids = verify_frozen_inputs(root, redactions)
    origin_counts, excerpt_verified, excerpt_missing = verify_evidence(root, redactions, case_ids)
    derived = verify_outcomes(root, redactions, case_ids)
    sensitivities = verify_development_sensitivities(root, redactions)

    observed = root / "source" / "SemWeaver" / "experiments" / "knighter" / "experiment"
    knighter = load(observed / "observed_matched_knighter_cohort.json")
    matched = load(observed / "observed_semweaver_matched_metamorphic_v3.json")
    full = load(observed / "observed_semweaver_full_budget_v3.json")
    targeted = load(observed / "observed_semweaver_targeted_postgate_v2.json")
    burden = load(observed / "observed_deploy_or_fallback_alert_burden.json")
    provenance = load(
        root / "evidence" / "semweaver_treatment_evidence_cohort_v3" / "PROVENANCE_AUDIT.json"
    )["summary"]
    binding = load(
        root / "evidence" / "semweaver_treatment_evidence_cohort_v3" / "BINDING_AUDIT.json"
    )

    require(knighter["subjects"] == 12 and knighter["accepted_refinements"] == 1, "Knighter outcome drift")
    require(knighter["accepted_candidate"]["metamorphic_robust"] is True, "Knighter RRS drift")
    require(matched["rrs"] is False, "matched SemWeaver RRS drift")
    require(full["pds"]["accepted"] == 4 and full["rrs"]["accepted"] == 0, "full-budget outcome drift")
    require(targeted["cases"][1]["fresh_confirmatory_suite"]["robust"] is False, "fresh-suite drift")
    require(provenance["total_records"] == 70, "evidence denominator drift")
    require(provenance["origin_counts"]["analyzer_internal"] == 37, "internal-evidence count drift")
    require(binding["passed"] is True and binding["bound_internal_records"] == 37, "binding audit drift")
    require(burden["starting_fixed_alerts"] == 37, "starting alert denominator drift")
    burden_by_condition = {item["condition"]: item for item in burden["conditions"]}
    require(burden_by_condition["knighter_model_matched"]["fixed_alerts_retained"] == 36, "Knighter alert burden drift")
    require(burden_by_condition["semweaver_call_matched"]["fixed_alerts_retained"] == 36, "matched SemWeaver alert burden drift")
    require(burden_by_condition["semweaver_full_budget"]["fixed_alerts_retained"] == 28, "full-budget alert burden drift")
    require(origin_counts["analyzer_internal"] == 37, "recomputed origin drift")
    require(excerpt_verified == 34 and excerpt_missing == 3, "source excerpt coverage drift")
    require(derived == {"knighter_pds": 1, "matched_pds": 1, "full_pds": 4, "fixed_retained": 28}, "recomputed outcome drift")

    print(
        json.dumps(
            {
                "status": "verified",
                "files": manifest["file_count"],
                "subjects": 12,
                "matched_pds": {"knighter": 1, "semweaver": 1},
                "suite_observations_not_method_comparison": {
                    "knighter_case05": "passed its mechanism-specific suite",
                    "semweaver_case19": "failed its mechanism-specific suites",
                },
                "posthoc_fixture_sensitivities_not_confirmatory": sensitivities,
                "full_budget": {"pds": 4, "rrs": 0},
                "deploy_or_fallback_fixed_alerts": {
                    "starting": 37,
                    "knighter_matched": 36,
                    "semweaver_matched": 36,
                    "semweaver_full_budget": 28,
                },
                "evidence_records": {
                    "total": 70,
                    "analyzer_internal": 37,
                    "bound": 37,
                    "source_excerpt_verified": excerpt_verified,
                    "source_excerpt_missing": excerpt_missing,
                },
            },
            indent=2,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
