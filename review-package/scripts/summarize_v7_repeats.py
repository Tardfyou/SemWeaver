#!/usr/bin/env python3
"""Summarize V7 repeated decodes without pooling subjects as independent."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import statistics
from pathlib import Path


REPLICATES = ("rep01", "rep02", "rep03")


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def summary(values: list[int]) -> dict:
    return {
        "values_by_replicate": values,
        "mean": statistics.mean(values),
        "range": [min(values), max(values)],
        "sample_sd": statistics.stdev(values),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repeats-root", required=True, type=Path)
    parser.add_argument("--screening", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    root = args.repeats_root.resolve()
    package_root = root.parents[1]
    redaction_path = package_root / "REDACTIONS.json"
    redactions = {
        row["path"]: row for row in load(redaction_path)["files"]
    } if redaction_path.is_file() else {}

    def digest(path: Path) -> str:
        actual = sha(path)
        if redactions and path.resolve().is_relative_to(package_root):
            relative = path.resolve().relative_to(package_root).as_posix()
            row = redactions.get(relative)
            if row:
                assert actual == row["redacted_sha256"], f"redacted file drift: {relative}"
                return str(row["original_sha256"])
        return actual

    with args.screening.open(newline="", encoding="utf-8-sig") as handle:
        screening = list(csv.DictReader(handle))
    assert len(screening) == 39
    fixed_by_commit = {row["commit_id"]: int(row["fixed_alerts"]) for row in screening}
    assert len(fixed_by_commit) == 39

    rows = []
    success_by_case: dict[str, dict[str, int]] = {}
    input_hashes = {"screening": digest(args.screening), "script": sha(Path(__file__))}
    for replicate in REPLICATES:
        rep = root / replicate
        strict_path = rep / "knighter_strict_pds" / "STRICT_RESULT.json"
        strict = load(strict_path)
        assert strict["subjects"] == 12 and strict["unscored"] == 0
        # Rep01 treatments were frozen against the pre-amendment strict
        # summary; the fresh diagnostic-policy revalidation is equivalent on
        # all behavior labels and call caps, but has a different file hash.
        if replicate == "rep01":
            bound_strict_path = root.parent / "repeats_v6" / "rep01" / "knighter_strict" / "STRICT_RESULT.json"
            if not bound_strict_path.is_file():
                bound_strict_path = rep / "knighter_strict_original" / "STRICT_RESULT.json"
        else:
            bound_strict_path = strict_path
        bound_strict = load(bound_strict_path)
        assert bound_strict["strict_pds"] == strict["strict_pds"]
        assert bound_strict["baseline_model_calls"] == strict["baseline_model_calls"]
        assert {
            row["case_id"]: (row["strict_pds"], row["llm_calls"])
            for row in bound_strict["rows"]
        } == {
            row["case_id"]: (row["strict_pds"], row["llm_calls"])
            for row in strict["rows"]
        }
        baseline = {row["case_id"]: row for row in strict["rows"]}
        assert len(baseline) == 12
        native_path = rep / "native" / "BATCH_RESULT.json"
        ablation_path = rep / "no_internal" / "BATCH_RESULT.json"
        native, ablation = load(native_path), load(ablation_path)
        manifests = {
            arm: load(rep / arm / "BATCH_MANIFEST.json")
            for arm in ("native", "no_internal")
        }
        assert manifests["native"]["cases"] == manifests["no_internal"]["cases"]
        assert manifests["native"]["total_model_call_cap"] == manifests["no_internal"]["total_model_call_cap"]
        assert manifests["native"]["baseline_summary_sha256"] == digest(bound_strict_path)
        assert manifests["no_internal"]["baseline_summary_sha256"] == digest(bound_strict_path)
        assert strict["baseline_model_calls"] == manifests["native"]["total_model_call_cap"]

        arm_rows = {}
        for arm, result in (("native", native), ("no_internal", ablation)):
            assert result["requested_cases"] == result["completed_records"] == 12
            assert result["batch_manifest_sha256"] == digest(rep / arm / "BATCH_MANIFEST.json")
            assert result["llm_calls"] <= strict["baseline_model_calls"]
            assert not (rep / arm / "BATCH_INTERRUPTED.json").exists()
            arm_rows[arm] = {row["case_id"]: row for row in result["rows"]}
            assert len(arm_rows[arm]) == 12
            assert set(arm_rows[arm]) == set(baseline)
            assert sum(row["accepted_pds"] is True for row in result["rows"]) == result["accepted_pds"]
        assert sum(row["llm_calls"] for row in baseline.values()) == strict["baseline_model_calls"]

        fixed_by_case = {}
        for case_id in baseline:
            prefix = case_id.split("_", 2)[1]
            hits = [value for commit, value in fixed_by_commit.items() if commit.startswith(prefix)]
            assert len(hits) == 1
            fixed_by_case[case_id] = hits[0]
        assert sum(fixed_by_case.values()) == 37

        sets = {
            "knighter": {case_id for case_id, row in baseline.items() if row["strict_pds"] is True},
            "native": {case_id for case_id, row in arm_rows["native"].items() if row["accepted_pds"] is True},
            "no_internal": {case_id for case_id, row in arm_rows["no_internal"].items() if row["accepted_pds"] is True},
        }
        assert len(sets["knighter"]) == strict["strict_pds"]
        review_gated = {
            case_id for case_id, row in baseline.items()
            if row["strict_pds"] is True and row.get("adoptable_with_review_gate") is True
        }
        arm_review_gated = {}
        for arm in ("native", "no_internal"):
            accepted = set()
            for case_id in sets[arm]:
                selected = arm_rows[arm][case_id]["selected_attempt"]
                assert isinstance(selected, int) and selected > 0
                validation_path = rep / arm / case_id / f"attempt-{selected:02d}" / "frozen_validation" / "RESULT.json"
                validation = load(validation_path)
                assert validation["pds"] is True and validation["execution_valid"] is True
                review_adoptable = validation.get("adoptable_with_review_gate")
                if review_adoptable is None:
                    # Rep01 pre-amendment validators gated PDS on review.
                    review_adoptable = bool(validation["pds"] and (validation.get("artifact_review") or {}).get("passed"))
                if review_adoptable is True:
                    accepted.add(case_id)
            arm_review_gated[arm] = accepted

        retained = {
            arm: 37 - sum(fixed_by_case[case_id] for case_id in cases)
            for arm, cases in sets.items()
        }
        for case_id in baseline:
            counter = success_by_case.setdefault(case_id, {arm: 0 for arm in sets})
            for arm, cases in sets.items():
                counter[arm] += case_id in cases
        rows.append({
            "replicate": replicate,
            "subjects": 12,
            "matched_call_cap": strict["baseline_model_calls"],
            "actual_calls": {
                "knighter": strict["baseline_model_calls"],
                "native": native["llm_calls"],
                "no_internal": ablation["llm_calls"],
            },
            "behavior_pds": {arm: len(cases) for arm, cases in sets.items()},
            "review_gated_pds": {
                "knighter": len(review_gated),
                **{arm: len(cases) for arm, cases in arm_review_gated.items()},
            },
            "retained_fixed_alerts": retained,
            "hypothetical_version_f1_pds_only": {
                arm: 78 / (78 + 12 - len(cases)) for arm, cases in sets.items()
            },
            "knighter_only": sorted(sets["knighter"] - sets["native"]),
            "native_only": sorted(sets["native"] - sets["knighter"]),
            "native_only_vs_ablation": sorted(sets["native"] - sets["no_internal"]),
            "ablation_only_vs_native": sorted(sets["no_internal"] - sets["native"]),
        })
        input_hashes[replicate] = {
            "strict": digest(strict_path),
            "strict_original_manifest_binding": digest(bound_strict_path),
            "native_manifest": digest(rep / "native" / "BATCH_MANIFEST.json"),
            "native_result": digest(native_path),
            "no_internal_manifest": digest(rep / "no_internal" / "BATCH_MANIFEST.json"),
            "no_internal_result": digest(ablation_path),
        }

    arms = ("knighter", "native", "no_internal")
    output = {
        "schema_version": 1,
        "status": "completed_descriptive_repeated_decode_summary",
        "replicate_count": 3,
        "subjects_per_replicate": 12,
        "distinct_subjects": 12,
        "input_sha256": input_hashes,
        "replicates": rows,
        "descriptive_across_replicates": {
            "behavior_pds": {arm: summary([row["behavior_pds"][arm] for row in rows]) for arm in arms},
            "retained_fixed_alerts": {arm: summary([row["retained_fixed_alerts"][arm] for row in rows]) for arm in arms},
            "actual_calls": {arm: summary([row["actual_calls"][arm] for row in rows]) for arm in arms},
        },
        "per_case_success_frequency_out_of_three": success_by_case,
        "inference_boundary": (
            "Three correlated model decodes on the same development-informed 12 cases; "
            "not 36 independent subjects, not a population prevalence estimate, "
            "and not evidence of unseen-code generalization."
        ),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(output, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps({"replicates": rows, "descriptive_across_replicates": output["descriptive_across_replicates"]}, indent=2))


if __name__ == "__main__":
    main()
