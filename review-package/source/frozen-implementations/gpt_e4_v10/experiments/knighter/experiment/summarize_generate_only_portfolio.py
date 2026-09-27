#!/usr/bin/env python3
"""Score only complete, independently replayed 39-case CSA portfolios."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _counts(rows: list[dict]) -> dict:
    tp = sum(row["vulnerable_alerts"] > 0 for row in rows)
    fn = len(rows) - tp
    fp = sum(row["fixed_alerts"] > 0 for row in rows)
    tn = len(rows) - fp
    denominator = 2 * tp + fp + fn
    return {
        "patch_pairs": len(rows), "patch_version_decisions": 2 * len(rows),
        "TP": tp, "FP": fp, "FN": fn, "TN": tn,
        "PDS": sum(row["vulnerable_alerts"] > 0 and row["fixed_alerts"] == 0 for row in rows),
        "fixed_object_alerts": sum(row["fixed_alerts"] for row in rows),
        "precision": tp / (tp + fp) if tp + fp else None,
        "recall": tp / (tp + fn) if tp + fn else None,
        "F1": 2 * tp / denominator if denominator else None,
    }


def summarize(screen_path: Path, precision_dir: Path, recovery_dir: Path) -> dict:
    screen = json.loads(screen_path.read_text(encoding="utf-8"))
    starting = {row["case_id"]: row for row in screen["rows"]}
    if screen.get("requested_cases") != 39 or screen.get("completed_records") != 39 or len(starting) != 39:
        raise ValueError("A complete, unique 39-case starting screen is required")
    if any(not row.get("execution_valid", False) for row in starting.values()):
        raise ValueError("Starting screen contains invalid execution")
    treatment_rows = {}
    batch_bindings = {}
    for stratum, directory in (("precision", precision_dir), ("recovery", recovery_dir)):
        manifest_path = directory / "BATCH_MANIFEST.json"
        result_path = directory / "BATCH_RESULT.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        result = json.loads(result_path.read_text(encoding="utf-8"))
        expected = list(manifest["execution_order"])
        actual = [row["case_id"] for row in result["rows"]]
        if (
            result.get("requested_cases") != len(expected)
            or result.get("completed_records") != len(expected)
            or actual != expected
            or result.get("batch_manifest_sha256") != sha256(manifest_path)
        ):
            raise ValueError(f"{stratum} batch incomplete or manifest-unbound")
        if stratum == "precision" and manifest.get("objective") != "precision_refine":
            raise ValueError("Precision treatment has the wrong objective")
        if stratum == "recovery" and manifest.get("objective") != "target_hit_recovery":
            raise ValueError("Recovery treatment has the wrong objective")
        for row in result["rows"]:
            case_id = row["case_id"]
            if case_id in treatment_rows or case_id not in starting:
                raise ValueError(f"Duplicate or unexpected treatment case: {case_id}")
            original = starting[case_id]
            if stratum == "precision" and not (original["vulnerable_alerts"] > 0 and original["fixed_alerts"] > 0):
                raise ValueError(f"Precision stratum mismatch: {case_id}")
            if stratum == "recovery" and original["vulnerable_alerts"] != 0:
                raise ValueError(f"Recovery stratum mismatch: {case_id}")
            treatment_rows[case_id] = (stratum, directory, row)
        batch_bindings[stratum] = {
            "batch_manifest_sha256": sha256(manifest_path),
            "batch_result_sha256": sha256(result_path),
            "cases": len(expected),
            "method": manifest["method"],
        }
    if set(treatment_rows) != set(starting):
        raise ValueError("Treatment batches do not partition the 39 starting cases")
    scored = []
    for case_id, original in starting.items():
        stratum, directory, treatment = treatment_rows[case_id]
        vuln = int(original["vulnerable_alerts"])
        fixed = int(original["fixed_alerts"])
        validated_attempts = []
        for attempt in treatment.get("attempt_rows", []):
            expected_hash = str(attempt.get("validation_result_sha256", "") or "")
            if not expected_hash:
                continue
            attempt_number = int(attempt["attempt"])
            validation_path = directory / case_id / f"attempt-{attempt_number:02d}" / "frozen_validation" / "RESULT.json"
            if sha256(validation_path) != expected_hash:
                raise ValueError(f"Paired validation hash mismatch: {case_id} attempt {attempt_number}")
            validation = json.loads(validation_path.read_text(encoding="utf-8"))
            if validation.get("candidate_sha256") != attempt.get("candidate_sha256"):
                raise ValueError(f"Candidate binding mismatch: {case_id} attempt {attempt_number}")
            if validation.get("execution_valid", False):
                validated_attempts.append((attempt_number, validation))
        hit_preserving = [
            (number, validation) for number, validation in validated_attempts
            if int(validation["vulnerable_alerts"]) > 0
        ]
        best = min(
            hit_preserving,
            key=lambda item: (int(item[1]["fixed_alerts"]), -int(item[1]["vulnerable_alerts"]), item[0]),
            default=None,
        )
        selected = None
        if treatment.get("accepted_pds", False):
            selected = int(treatment["selected_attempt"])
            matching = [row for row in treatment["attempt_rows"] if row["attempt"] == selected]
            if len(matching) != 1 or matching[0].get("outcome") != "refined_pds":
                raise ValueError(f"Accepted attempt is not uniquely bound: {case_id}")
            selected_rows = [validation for number, validation in validated_attempts if number == selected]
            if len(selected_rows) != 1:
                raise ValueError(f"Selected attempt has no valid paired replay: {case_id}")
            validation = selected_rows[0]
            if (
                not validation.get("pds", False)
                or validation.get("candidate_sha256") != matching[0]["candidate_sha256"]
            ):
                raise ValueError(f"Accepted candidate lacks independently bound PDS: {case_id}")
            vuln = int(validation["vulnerable_alerts"])
            fixed = int(validation["fixed_alerts"])
        scored.append({
            "case_id": case_id, "stratum": stratum,
            "starting_vulnerable_alerts": int(original["vulnerable_alerts"]),
            "starting_fixed_alerts": int(original["fixed_alerts"]),
            "vulnerable_alerts": vuln, "fixed_alerts": fixed,
            "accepted_pds": bool(treatment.get("accepted_pds", False)),
            "selected_attempt": selected,
            "valid_paired_attempts": len(validated_attempts),
            "best_hit_preserving_attempt": best[0] if best else None,
            "best_hit_preserving_vulnerable_alerts": int(best[1]["vulnerable_alerts"]) if best else None,
            "best_hit_preserving_fixed_alerts": int(best[1]["fixed_alerts"]) if best else None,
            "hit_preserving_fixed_alert_reduction": (
                max(0, int(original["fixed_alerts"]) - int(best[1]["fixed_alerts"]))
                if best and stratum == "precision" else 0
            ),
            "terminal_outcome": treatment.get("outcome"),
            "llm_calls": int(treatment.get("llm_calls", 0)),
        })
    baseline_rows = [
        {"vulnerable_alerts": row["starting_vulnerable_alerts"],
         "fixed_alerts": row["starting_fixed_alerts"]}
        for row in scored
    ]
    return {
        "schema_version": 1,
        "method": "complete_39_case_adopt_only_on_independent_pds",
        "screen_result_sha256": sha256(screen_path),
        "batches": batch_bindings,
        "baseline": _counts(baseline_rows),
        "post_adoption": _counts(scored),
        "diagnostic_candidate_effects": {
            "precision_cases_with_any_hit_preserving_fixed_reduction": sum(
                row["stratum"] == "precision" and row["hit_preserving_fixed_alert_reduction"] > 0
                for row in scored
            ),
            "precision_cases_with_partial_nonzero_fixed_reduction": sum(
                row["stratum"] == "precision"
                and row["hit_preserving_fixed_alert_reduction"] > 0
                and row["best_hit_preserving_fixed_alerts"] > 0
                for row in scored
            ),
            "precision_best_observed_fixed_alert_reduction_sum": sum(
                row["hit_preserving_fixed_alert_reduction"] for row in scored
            ),
            "recovery_cases_with_any_vulnerable_hit": sum(
                row["stratum"] == "recovery" and row["best_hit_preserving_attempt"] is not None
                for row in scored
            ),
            "recovery_cases_with_hit_but_residual_fixed_noise": sum(
                row["stratum"] == "recovery"
                and row["best_hit_preserving_fixed_alerts"] is not None
                and row["best_hit_preserving_fixed_alerts"] > 0
                for row in scored
            ),
            "interpretation": "Best observed validated candidates within frozen budgets; not adopted unless PDS.",
        },
        "strata": {
            name: {
                "baseline": _counts([baseline_rows[i] for i, row in enumerate(scored) if row["stratum"] == name]),
                "post_adoption": _counts([row for row in scored if row["stratum"] == name]),
            }
            for name in ("precision", "recovery")
        },
        "rows": scored,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--screen", required=True, type=Path)
    parser.add_argument("--precision-dir", required=True, type=Path)
    parser.add_argument("--recovery-dir", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    payload = summarize(args.screen.resolve(), args.precision_dir.resolve(), args.recovery_dir.resolve())
    output = args.output.resolve()
    if output.exists():
        raise FileExistsError(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(payload, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps({"baseline": payload["baseline"], "post_adoption": payload["post_adoption"]}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
