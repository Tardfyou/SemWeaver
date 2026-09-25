#!/usr/bin/env python3
"""Run SemWeaver with hash-bound paired feedback and frozen call caps.

The original full-budget profile remains available. With --baseline-summary,
each case inherits the successful model-call count of its paired KNighter run.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
import time
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
    if not validation or not validation.get("execution_valid", False):
        return "paired_validation_execution_error"
    if validation.get("pds", False):
        return "refined_pds"
    if not validation.get("vulnerable_hit", False) and validation.get("fixed_silent", False):
        return "validity_loss"
    if validation.get("vulnerable_hit", False) and not validation.get("fixed_silent", False):
        return "residual_fixed_noise"
    return "paired_behavior_failure"


def can_continue_failed_candidate(
    treatment: dict, *, manifest_exists: bool, candidate_exists: bool
) -> bool:
    return bool(
        treatment
        and not treatment.get("success", False)
        and manifest_exists
        and candidate_exists
        and str(treatment.get("candidate_sha256", "") or "")
    )


def selected_outcome(attempt_rows: list[dict]) -> tuple[str, int | None]:
    for row in reversed(attempt_rows):
        if row.get("validation_result_sha256"):
            return str(row.get("outcome", "") or ""), int(row.get("attempt", 0) or 0)
    if attempt_rows:
        return str(attempt_rows[-1].get("outcome", "") or ""), None
    return "no_candidate_within_budget", None


def matched_call_budgets(baseline: dict, case_ids: list[str]) -> dict[str, int]:
    rows = list(baseline.get("rows", []) or [])
    if baseline.get("subjects") != len(case_ids) or baseline.get("unscored") != 0:
        raise ValueError("matched baseline is incomplete or has unscored subjects")
    budgets = {str(row.get("case_id", "")): int(row.get("llm_calls", 0) or 0) for row in rows}
    if len(rows) != len(budgets) or set(budgets) != set(case_ids):
        raise ValueError("matched baseline case identities differ from evidence cohort")
    if any(value <= 0 for value in budgets.values()):
        raise ValueError("every matched case must have a positive call cap")
    if sum(budgets.values()) != int(baseline.get("baseline_model_calls", -1)):
        raise ValueError("matched baseline call denominator drift")
    return budgets


def provider_interruption_events(path: Path) -> list[str]:
    if not path.is_file():
        return []
    interrupting = {"model_rate_limit_retry", "model_rate_limit_exhausted", "model_bind_fallback"}
    found = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        event = json.loads(line)
        name = str(event.get("event", ""))
        if name in interrupting:
            found.append(name)
    return found


def rate_limit_observed(text: str) -> bool:
    return bool(re.search(
        r"gateway_concurrency_limit|rate_limit_error|error code:\s*429|http\s*429",
        text,
        flags=re.IGNORECASE,
    ))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--cases-root", required=True, type=Path)
    parser.add_argument("--evidence-batch", required=True, type=Path)
    parser.add_argument("--binding-audit", required=True, type=Path)
    parser.add_argument("--linux-dir", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--backend-root", required=True, type=Path)
    parser.add_argument("--baseline-summary", type=Path,
                        help="Strict KNighter result; enables per-case matched call caps")
    parser.add_argument("--evidence-mode", choices=("native", "no_internal"), default="native")
    parser.add_argument("--call-cap", type=int, default=8)
    parser.add_argument("--attempt-cap", type=int, default=4)
    parser.add_argument("--max-tokens", type=int, default=16000)
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--freeze-only", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.call_cap <= 0 or args.attempt_cap <= 0:
        raise ValueError("call caps must be positive")
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
    evidence_rows = list(evidence_result.get("rows", []) or [])
    if any(not row.get("eligible", False) for row in evidence_rows):
        raise ValueError("ineligible evidence row reached treatment")
    case_ids = [str(row.get("case_id", "")) for row in evidence_rows]
    if not case_ids or len(case_ids) != len(set(case_ids)):
        raise ValueError("evidence cohort has missing or duplicate case identities")

    baseline_path = args.baseline_summary.resolve() if args.baseline_summary else None
    baseline = json.loads(baseline_path.read_text(encoding="utf-8")) if baseline_path else None
    if baseline is not None and args.attempt_cap != 1:
        raise ValueError("Matched feedback profile requires --attempt-cap 1 so every candidate is paired-validated before another model call")
    call_caps = (
        matched_call_budgets(baseline, case_ids)
        if baseline is not None
        else {case_id: args.call_cap for case_id in case_ids}
    )

    revision = subprocess.run(
        ["git", "rev-parse", "HEAD"], cwd=str(PROJECT_ROOT),
        capture_output=True, text=True, check=True,
    ).stdout.strip()
    frozen_cases = []
    for case_id in case_ids:
        evidence_path = evidence_batch / case_id / "csa" / "evidence_bundle.json"
        frozen_cases.append({
            "case_id": case_id,
            "call_cap": call_caps[case_id],
            "patch_sha256": sha256(cases_root / case_id / "patches" / "commit.patch"),
            "starting_checker_sha256": sha256(cases_root / case_id / "csa" / "SAGenTestChecker.cpp"),
            "evidence_bundle_sha256": sha256(evidence_path),
        })
    batch_manifest = {
        "schema_version": 1,
        "method": (
            "semweaver_native_paired_feedback_matched"
            if baseline_path and args.evidence_mode == "native"
            else "semweaver_no_internal_paired_feedback_matched"
            if baseline_path
            else "semweaver_provenance_gated_csa_full_budget_treatment"
        ),
        "evidence_mode": args.evidence_mode,
        "implementation_revision": revision,
        "evidence_batch_manifest_sha256": sha256(evidence_manifest_path),
        "evidence_batch_result_sha256": sha256(evidence_result_path),
        "binding_audit_sha256": sha256(binding_path),
        "baseline_summary_sha256": sha256(baseline_path) if baseline_path else "",
        "model": baseline.get("model", "") if baseline else "",
        "wire_api": baseline.get("wire_api", "") if baseline else "",
        "reasoning_effort": baseline.get("reasoning_effort", "") if baseline else "",
        "call_cap_per_subject": args.call_cap if baseline is None else None,
        "attempt_cap": args.attempt_cap,
        "max_tokens_per_call": args.max_tokens,
        "total_model_call_cap": sum(call_caps.values()),
        "execution_order": case_ids,
        "cases": frozen_cases,
    }
    batch_manifest_path = output_dir / "BATCH_MANIFEST.json"
    if batch_manifest_path.exists():
        if json.loads(batch_manifest_path.read_text(encoding="utf-8")) != batch_manifest:
            raise RuntimeError("Existing feedback-treatment manifest differs from frozen inputs")
    else:
        batch_manifest_path.write_text(
            json.dumps(batch_manifest, indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8",
        )
    if args.freeze_only:
        print(json.dumps({
            "subjects": len(case_ids),
            "total_model_call_cap": batch_manifest["total_model_call_cap"],
            "evidence_mode": args.evidence_mode,
            "baseline_summary_sha256": batch_manifest["baseline_summary_sha256"],
        }, indent=2))
        return 0
    if (output_dir / "BATCH_INTERRUPTED.json").exists():
        raise RuntimeError("Interrupted treatment must restart in a fresh output directory")

    treatment_driver = HERE / "run_semweaver_treatment_case.py"
    validator = HERE / "validate_frozen_csa_candidate.py"
    rows = []
    for case_index, case_id in enumerate(case_ids, start=1):
        subject_cap = call_caps[case_id]
        case_root = output_dir / case_id
        case_root.mkdir(parents=True, exist_ok=True)
        evidence_path = evidence_batch / case_id / "csa" / "evidence_bundle.json"
        calls_used = 0
        attempt_rows = []
        next_feedback_result: Path | None = None
        next_feedback_manifest: Path | None = None
        next_starting_candidate: Path | None = None
        final_outcome = "no_candidate_within_budget"
        accepted_pds = False

        while calls_used < subject_cap and not accepted_pds:
            attempt_number = len(attempt_rows) + 1
            attempt_started = time.monotonic()
            attempt_output = case_root / f"attempt-{attempt_number:02d}"
            attempt_budget = min(args.attempt_cap, subject_cap - calls_used)
            command = [
                sys.executable, str(treatment_driver),
                "--config", str(args.config.resolve()),
                "--case-dir", str(cases_root / case_id),
                "--evidence-bundle", str(evidence_path),
                "--evidence-dir", str(args.linux_dir.resolve()),
                "--output-dir", str(attempt_output),
                "--max-iterations", str(attempt_budget),
                "--max-tokens", str(args.max_tokens),
                "--evidence-mode", args.evidence_mode,
            ]
            if next_feedback_result and next_starting_candidate:
                command.extend([
                    "--feedback-result", str(next_feedback_result),
                    "--starting-candidate", str(next_starting_candidate),
                ])
            elif next_feedback_manifest and next_starting_candidate:
                command.extend([
                    "--feedback-run-manifest", str(next_feedback_manifest),
                    "--starting-candidate", str(next_starting_candidate),
                ])
            treatment_run = subprocess.run(command, capture_output=True, text=True, check=False)
            prefix = output_dir / f"coordinator-{case_index:02d}-{case_id}-a{attempt_number:02d}"
            stdout_log = Path(f"{prefix}.treatment.stdout.log")
            stderr_log = Path(f"{prefix}.treatment.stderr.log")
            stdout_log.write_text(treatment_run.stdout, encoding="utf-8")
            stderr_log.write_text(treatment_run.stderr, encoding="utf-8")
            provider_events = provider_interruption_events(attempt_output / "run_events.jsonl")
            if (treatment_run.returncode == 75 or provider_events or
                    rate_limit_observed(treatment_run.stdout + treatment_run.stderr)):
                interruption = {
                    "schema_version": 1,
                    "status": "infrastructure_interrupted_before_method_outcome",
                    "reason": "provider_rate_limit_or_binding_fallback",
                    "batch_manifest_sha256": sha256(batch_manifest_path),
                    "case_id": case_id,
                    "attempt": attempt_number,
                    "completed_cases": len(rows),
                    "provider_events": provider_events,
                    "treatment_return_code": treatment_run.returncode,
                    "stdout_sha256": sha256(stdout_log),
                    "stderr_sha256": sha256(stderr_log),
                }
                (output_dir / "BATCH_INTERRUPTED.json").write_text(
                    json.dumps(interruption, indent=2, ensure_ascii=False) + "\n",
                    encoding="utf-8",
                )
                print(json.dumps(interruption, indent=2, ensure_ascii=False), file=sys.stderr)
                return 75
            treatment_manifest_path = attempt_output / "RUN_MANIFEST.json"
            if not treatment_manifest_path.is_file():
                interruption = {
                    "schema_version": 1,
                    "status": "execution_interrupted_before_method_outcome",
                    "reason": "missing_treatment_manifest",
                    "batch_manifest_sha256": sha256(batch_manifest_path),
                    "case_id": case_id,
                    "attempt": attempt_number,
                    "completed_cases": len(rows),
                    "treatment_return_code": treatment_run.returncode,
                    "stdout_sha256": sha256(stdout_log),
                    "stderr_sha256": sha256(stderr_log),
                }
                (output_dir / "BATCH_INTERRUPTED.json").write_text(
                    json.dumps(interruption, indent=2, ensure_ascii=False) + "\n",
                    encoding="utf-8",
                )
                print(json.dumps(interruption, indent=2, ensure_ascii=False), file=sys.stderr)
                return 74
            treatment = json.loads(treatment_manifest_path.read_text(encoding="utf-8"))
            if baseline is not None and (
                treatment.get("model") != baseline.get("model")
                or treatment.get("wire_api") != baseline.get("wire_api")
                or treatment.get("reasoning_effort") != baseline.get("reasoning_effort")
            ):
                raise RuntimeError(f"Model identity or effort drift for {case_id}")
            usage = dict(treatment.get("llm_usage", {}) or {})
            observed_calls = int(usage.get("call_count", 0) or 0)
            if observed_calls > attempt_budget:
                raise RuntimeError(f"Model-call cap exceeded for {case_id} attempt {attempt_number}")
            calls_used += observed_calls
            validation = None
            validation_path = attempt_output / "frozen_validation" / "RESULT.json"
            validation_return_code = None
            validation_seconds = 0.0
            candidate_path = attempt_output / "SAGenTestChecker.cpp"
            if treatment.get("success", False):
                validation_started = time.monotonic()
                validation_command = [
                    sys.executable, str(validator),
                    "--candidate", str(candidate_path),
                    "--case-dir", str(cases_root / case_id),
                    "--linux-dir", str(args.linux_dir.resolve()),
                    "--output-dir", str(attempt_output / "frozen_validation"),
                    "--backend-workspace", str(backend_root / case_id / f"attempt-{attempt_number:02d}"),
                    "--jobs", str(args.jobs),
                ]
                validation_run = subprocess.run(
                    validation_command, capture_output=True, text=True, check=False
                )
                validation_seconds = time.monotonic() - validation_started
                validation_return_code = validation_run.returncode
                Path(f"{prefix}.validation.stdout.log").write_text(validation_run.stdout, encoding="utf-8")
                Path(f"{prefix}.validation.stderr.log").write_text(validation_run.stderr, encoding="utf-8")
                if validation_path.is_file():
                    validation = json.loads(validation_path.read_text(encoding="utf-8"))

            outcome = classify_outcome(treatment, validation)
            final_outcome = outcome
            accepted_pds = outcome == "refined_pds"
            attempt_rows.append({
                "attempt": attempt_number,
                "call_cap": attempt_budget,
                "llm_calls": observed_calls,
                "prompt_tokens": int(usage.get("prompt_tokens", 0) or 0),
                "completion_tokens": int(usage.get("completion_tokens", 0) or 0),
                "total_tokens": int(usage.get("total_tokens", 0) or 0),
                "treatment_return_code": treatment_run.returncode,
                "validation_return_code": validation_return_code,
                "elapsed_seconds": time.monotonic() - attempt_started,
                "paired_validation_seconds": validation_seconds,
                "outcome": outcome,
                "candidate_sha256": str(treatment.get("candidate_sha256", "") or ""),
                "treatment_manifest_sha256": sha256(treatment_manifest_path) if treatment_manifest_path.is_file() else "",
                "validation_result_sha256": sha256(validation_path) if validation_path.is_file() else "",
            })
            if observed_calls <= 0:
                break
            if accepted_pds or calls_used >= subject_cap:
                break
            next_feedback_result = None
            next_feedback_manifest = None
            next_starting_candidate = None
            if treatment.get("success", False) and validation_path.is_file() and candidate_path.is_file():
                next_feedback_result = validation_path
                next_starting_candidate = candidate_path
            elif can_continue_failed_candidate(
                treatment,
                manifest_exists=treatment_manifest_path.is_file(),
                candidate_exists=candidate_path.is_file(),
            ):
                next_feedback_manifest = treatment_manifest_path
                next_starting_candidate = candidate_path

        retained_outcome, selected_attempt = selected_outcome(attempt_rows)
        rows.append({
            "case_id": case_id,
            "call_cap": subject_cap,
            "llm_calls": sum(row["llm_calls"] for row in attempt_rows),
            "prompt_tokens": sum(row["prompt_tokens"] for row in attempt_rows),
            "completion_tokens": sum(row["completion_tokens"] for row in attempt_rows),
            "total_tokens": sum(row["total_tokens"] for row in attempt_rows),
            "elapsed_seconds": sum(row["elapsed_seconds"] for row in attempt_rows),
            "paired_validation_seconds": sum(row["paired_validation_seconds"] for row in attempt_rows),
            "attempts": len(attempt_rows),
            "outcome": final_outcome if accepted_pds else retained_outcome,
            "terminal_outcome": final_outcome,
            "selected_attempt": selected_attempt,
            "accepted_pds": accepted_pds,
            "attempt_rows": attempt_rows,
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
            "elapsed_seconds": sum(row["elapsed_seconds"] for row in rows),
            "paired_validation_seconds": sum(row["paired_validation_seconds"] for row in rows),
            "outcomes": {
                label: sum(row["outcome"] == label for row in rows)
                for label in sorted({row["outcome"] for row in rows})
            },
            "rows": rows,
        }
        (output_dir / "BATCH_RESULT.json").write_text(
            json.dumps(partial, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
        )

    print(json.dumps(partial, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
