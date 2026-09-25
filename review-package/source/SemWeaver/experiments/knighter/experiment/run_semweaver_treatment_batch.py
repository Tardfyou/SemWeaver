#!/usr/bin/env python3
"""Run the frozen SemWeaver CSA treatment under matched per-case call caps."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path


HERE = Path(__file__).resolve().parent
PROJECT_ROOT = HERE.parents[2]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def classify_outcome(treatment: dict, validation: dict | None) -> str:
    if not treatment:
        return "treatment_execution_error"
    if not treatment.get("success", False):
        failure_type = str(treatment.get("failure_type", "") or "")
        if failure_type.startswith("preflight_"):
            return failure_type
        if int(treatment.get("compile_attempts", 0) or 0) > 0:
            return "candidate_compile_failure"
        if not treatment.get("candidate_changed", False):
            return "no_candidate_within_budget"
        return "candidate_not_locally_valid"
    if validation and validation.get("build_return_code") not in (None, 0):
        return "candidate_compile_failure_external"
    if not validation or not validation.get("execution_valid", False):
        return "paired_validation_execution_error"
    if validation.get("pds", False):
        return "refined_pds"
    if not validation.get("vulnerable_hit", False) and validation.get("fixed_silent", False):
        return "validity_loss"
    if validation.get("vulnerable_hit", False) and not validation.get("fixed_silent", False):
        return "residual_fixed_noise"
    return "paired_behavior_failure"


def provider_interruption_events(path: Path) -> list[str]:
    if not path.is_file():
        return []
    interrupting = {
        "model_rate_limit_retry",
        "model_rate_limit_exhausted",
        "model_bind_fallback",
    }
    events = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        event = json.loads(line)
        name = str(event.get("event", ""))
        if name in interrupting:
            events.append(name)
    return events


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--cases-root", required=True, type=Path)
    parser.add_argument("--evidence-batch", required=True, type=Path)
    parser.add_argument("--binding-audit", required=True, type=Path)
    parser.add_argument("--baseline-summary", required=True, type=Path)
    parser.add_argument("--linux-dir", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--backend-root", required=True, type=Path)
    parser.add_argument("--max-tokens", type=int, default=16000)
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--evidence-mode", choices=("native", "no_internal"), default="native")
    parser.add_argument("--freeze-only", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    cases_root = args.cases_root.resolve()
    evidence_batch = args.evidence_batch.resolve()
    output_dir = args.output_dir.resolve()
    backend_root = args.backend_root.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    backend_root.mkdir(parents=True, exist_ok=True)

    evidence_manifest_path = evidence_batch / "BATCH_MANIFEST.json"
    evidence_result_path = evidence_batch / "BATCH_RESULT.json"
    evidence_result = json.loads(evidence_result_path.read_text(encoding="utf-8"))
    binding_path = args.binding_audit.resolve()
    binding = json.loads(binding_path.read_text(encoding="utf-8"))
    if not binding.get("passed", False):
        raise ValueError("strict evidence binding audit did not pass")
    if binding.get("batch_manifest_sha256") != sha256(evidence_manifest_path):
        raise ValueError("binding audit does not match evidence batch manifest")
    if binding.get("batch_result_sha256") != sha256(evidence_result_path):
        raise ValueError("binding audit does not match evidence batch result")

    baseline_path = args.baseline_summary.resolve()
    baseline = json.loads(baseline_path.read_text(encoding="utf-8"))
    budgets = {
        str(row.get("case_id", "")): int(row.get("llm_calls", 0) or 0)
        for row in list(baseline.get("rows", []) or [])
    }
    evidence_rows = list(evidence_result.get("rows", []) or [])
    case_ids = [str(row.get("case_id", "")) for row in evidence_rows]
    if any(not case_id or not budgets.get(case_id) for case_id in case_ids):
        raise ValueError("missing positive matched call budget for an evidence case")
    if any(not row.get("eligible", False) for row in evidence_rows):
        raise ValueError("ineligible evidence row reached treatment batch")

    revision = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=str(PROJECT_ROOT),
        capture_output=True,
        text=True,
        check=True,
    ).stdout.strip()
    frozen_cases = []
    for case_id in case_ids:
        evidence_path = evidence_batch / case_id / "csa" / "evidence_bundle.json"
        frozen_cases.append({
            "case_id": case_id,
            "max_model_calls": budgets[case_id],
            "case_patch_sha256": sha256(cases_root / case_id / "patches" / "commit.patch"),
            "starting_checker_sha256": sha256(cases_root / case_id / "csa" / "SAGenTestChecker.cpp"),
            "evidence_bundle_sha256": sha256(evidence_path),
        })
    batch_manifest = {
        "schema_version": 1,
        "method": (
            "semweaver_provenance_gated_csa_matched_treatment"
            if args.evidence_mode == "native"
            else "semweaver_no_internal_evidence_matched_ablation"
        ),
        "evidence_mode": args.evidence_mode,
        "implementation_revision": revision,
        "evidence_batch_manifest_sha256": sha256(evidence_manifest_path),
        "evidence_batch_result_sha256": sha256(evidence_result_path),
        "binding_audit_sha256": sha256(binding_path),
        "baseline_summary_sha256": sha256(baseline_path),
        "model": baseline.get("model", ""),
        "wire_api": baseline.get("wire_api", ""),
        "reasoning_effort": baseline.get("reasoning_effort", ""),
        "max_tokens_per_call": args.max_tokens,
        "total_model_call_cap": sum(budgets[case_id] for case_id in case_ids),
        "execution_order": case_ids,
        "cases": frozen_cases,
    }
    batch_manifest_path = output_dir / "BATCH_MANIFEST.json"
    if batch_manifest_path.exists():
        previous = json.loads(batch_manifest_path.read_text(encoding="utf-8"))
        if previous != batch_manifest:
            raise RuntimeError("Existing matched treatment manifest differs from requested frozen run")
    else:
        batch_manifest_path.write_text(
            json.dumps(batch_manifest, indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8",
        )
    if args.freeze_only:
        print(json.dumps(batch_manifest, indent=2, ensure_ascii=False))
        return 0
    if (output_dir / "BATCH_INTERRUPTED.json").exists():
        raise RuntimeError("Rate-limited batch must restart in a fresh output directory")

    treatment_driver = HERE / "run_semweaver_treatment_case.py"
    validator = HERE / "validate_frozen_csa_candidate.py"
    rows = []
    for index, case_id in enumerate(case_ids, start=1):
        case_output = output_dir / case_id
        case_output.mkdir(parents=True, exist_ok=True)
        evidence_path = evidence_batch / case_id / "csa" / "evidence_bundle.json"
        treatment_command = [
            sys.executable,
            str(treatment_driver),
            "--config",
            str(args.config.resolve()),
            "--case-dir",
            str(cases_root / case_id),
            "--evidence-bundle",
            str(evidence_path),
            "--evidence-dir",
            str(args.linux_dir.resolve()),
            "--output-dir",
            str(case_output),
            "--max-iterations",
            str(budgets[case_id]),
            "--max-tokens",
            str(args.max_tokens),
            "--evidence-mode",
            args.evidence_mode,
        ]
        treatment_run = subprocess.run(
            treatment_command, capture_output=True, text=True, check=False
        )
        (output_dir / f"coordinator-{index:02d}-{case_id}.treatment.stdout.log").write_text(
            treatment_run.stdout, encoding="utf-8"
        )
        (output_dir / f"coordinator-{index:02d}-{case_id}.treatment.stderr.log").write_text(
            treatment_run.stderr, encoding="utf-8"
        )
        events_path = case_output / "run_events.jsonl"
        provider_events = provider_interruption_events(events_path)
        if treatment_run.returncode == 75 or provider_events:
            interruption_path = case_output / "INFRASTRUCTURE_INTERRUPTION.json"
            interruption = {
                "schema_version": 1,
                "status": "batch_interrupted_before_method_outcome",
                "reason": (
                    "model_rate_limit_exhausted"
                    if treatment_run.returncode == 75
                    else "provider_rate_limit_or_response_format_drift"
                ),
                "batch_manifest_sha256": sha256(batch_manifest_path),
                "case_id": case_id,
                "case_index": index,
                "completed_records": len(rows),
                "provider_events": provider_events,
                "case_interruption_sha256": (
                    sha256(interruption_path) if interruption_path.is_file() else ""
                ),
            }
            (output_dir / "BATCH_INTERRUPTED.json").write_text(
                json.dumps(interruption, indent=2, ensure_ascii=False) + "\n",
                encoding="utf-8",
            )
            print(json.dumps(interruption, indent=2, ensure_ascii=False), file=sys.stderr)
            return 75
        treatment_manifest_path = case_output / "RUN_MANIFEST.json"
        treatment = (
            json.loads(treatment_manifest_path.read_text(encoding="utf-8"))
            if treatment_manifest_path.is_file()
            else {}
        )
        validation = None
        validation_path = case_output / "frozen_validation" / "RESULT.json"
        validation_return_code = None
        if treatment.get("success", False):
            validation_command = [
                sys.executable,
                str(validator),
                "--candidate",
                str(case_output / "SAGenTestChecker.cpp"),
                "--case-dir",
                str(cases_root / case_id),
                "--linux-dir",
                str(args.linux_dir.resolve()),
                "--output-dir",
                str(case_output / "frozen_validation"),
                "--backend-workspace",
                str(backend_root / case_id),
                "--jobs",
                str(args.jobs),
            ]
            validation_run = subprocess.run(
                validation_command, capture_output=True, text=True, check=False
            )
            validation_return_code = validation_run.returncode
            (output_dir / f"coordinator-{index:02d}-{case_id}.validation.stdout.log").write_text(
                validation_run.stdout, encoding="utf-8"
            )
            (output_dir / f"coordinator-{index:02d}-{case_id}.validation.stderr.log").write_text(
                validation_run.stderr, encoding="utf-8"
            )
            if validation_path.is_file():
                validation = json.loads(validation_path.read_text(encoding="utf-8"))

        usage = dict(treatment.get("llm_usage", {}) or {})
        outcome = classify_outcome(treatment, validation)
        observed_calls = int(usage.get("call_count", 0) or 0)
        if observed_calls > budgets[case_id]:
            outcome = "model_call_cap_exceeded"
        elif treatment and str(treatment.get("evidence_mode", "")) != args.evidence_mode:
            outcome = "evidence_mode_mismatch"
        elif treatment and (
            str(treatment.get("model", "")) != str(baseline.get("model", ""))
            or str(treatment.get("wire_api", "")) != str(baseline.get("wire_api", ""))
        ):
            outcome = "model_identity_mismatch"
        rows.append({
            "case_id": case_id,
            "max_model_calls": budgets[case_id],
            "treatment_return_code": treatment_run.returncode,
            "validation_return_code": validation_return_code,
            "outcome": outcome,
            "accepted_pds": outcome == "refined_pds",
            "llm_calls": observed_calls,
            "prompt_tokens": int(usage.get("prompt_tokens", 0) or 0),
            "completion_tokens": int(usage.get("completion_tokens", 0) or 0),
            "total_tokens": int(usage.get("total_tokens", 0) or 0),
            "treatment_manifest_sha256": (
                sha256(treatment_manifest_path) if treatment_manifest_path.is_file() else ""
            ),
            "validation_result_sha256": (
                sha256(validation_path) if validation_path.is_file() else ""
            ),
        })
        partial = {
            "schema_version": 1,
            "method": batch_manifest["method"],
            "batch_manifest_sha256": sha256(batch_manifest_path),
            "requested_cases": len(case_ids),
            "completed_records": len(rows),
            "accepted_pds": sum(row["accepted_pds"] for row in rows),
            "llm_calls": sum(row["llm_calls"] for row in rows),
            "prompt_tokens": sum(row["prompt_tokens"] for row in rows),
            "completion_tokens": sum(row["completion_tokens"] for row in rows),
            "total_tokens": sum(row["total_tokens"] for row in rows),
            "outcomes": {
                label: sum(row["outcome"] == label for row in rows)
                for label in sorted({row["outcome"] for row in rows})
            },
            "rows": rows,
        }
        (output_dir / "BATCH_RESULT.json").write_text(
            json.dumps(partial, indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8",
        )

    print(json.dumps(partial, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
