#!/usr/bin/env python3
"""Run one frozen provenance-gated SemWeaver CSA treatment candidate."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(PROJECT_ROOT))

from src.refine import LangChainRefinementAgent, RefinementRequest
from src.refine.agent import ModelRateLimitExhausted
from src.tools import ToolProviderOptions, build_tool_registry
from src.utils import load_config


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def treatment_evidence_view(bundle: dict, evidence_mode: str) -> tuple[dict, bool]:
    """Return exactly the evidence visible to the agent and origin-gate state."""
    if evidence_mode == "native":
        return bundle, True
    if evidence_mode == "no_internal":
        return {"records": [], "missing_evidence": [], "collected_analyzers": []}, False
    raise ValueError(f"Unsupported evidence mode: {evidence_mode}")


def patch_source_paths(patch_text: str) -> list[str]:
    paths = []
    for line in str(patch_text or "").splitlines():
        if not line.startswith("--- a/"):
            continue
        value = line.removeprefix("--- a/").strip()
        path = Path(value)
        if value and not path.is_absolute() and ".." not in path.parts and value not in paths:
            paths.append(value)
    return paths


def materialize_frozen_source(
    *, repo_root: Path, revision: str, patch_text: str, output_root: Path
) -> dict[str, str]:
    hashes = {}
    for relative in patch_source_paths(patch_text):
        completed = subprocess.run(
            ["git", "show", f"{revision}:{relative}"],
            cwd=str(repo_root),
            capture_output=True,
            check=False,
        )
        if completed.returncode != 0:
            raise RuntimeError(
                f"failed to materialize {relative} at {revision}: "
                f"{completed.stderr.decode('utf-8', errors='ignore')[:500]}"
            )
        destination = output_root / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(completed.stdout)
        hashes[relative] = sha256(destination)
    if not hashes:
        raise ValueError("patch contains no materializable pre-patch source files")
    return hashes


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--case-dir", required=True, type=Path)
    parser.add_argument("--evidence-bundle", required=True, type=Path)
    parser.add_argument("--evidence-dir", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument(
        "--feedback-result",
        type=Path,
        help="Optional frozen validation RESULT.json from the preceding automatic attempt",
    )
    parser.add_argument(
        "--feedback-run-manifest",
        type=Path,
        help="Optional failed treatment RUN_MANIFEST.json for an automatic repair continuation",
    )
    parser.add_argument(
        "--starting-candidate",
        type=Path,
        help="Candidate bound by --feedback-run-manifest; both options must be supplied together",
    )
    parser.add_argument("--max-iterations", type=int, default=8)
    parser.add_argument("--max-tokens", type=int, default=16000)
    parser.add_argument("--evidence-mode", choices=("native", "no_internal"), default="native")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.max_iterations <= 0 or args.max_tokens <= 0:
        raise ValueError("Budgets must be positive")
    case_dir = args.case_dir.resolve()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    patch_path = case_dir / "patches" / "commit.patch"
    original_source_path = case_dir / "csa" / "SAGenTestChecker.cpp"
    if args.feedback_run_manifest and not args.starting_candidate:
        raise ValueError("--feedback-run-manifest requires --starting-candidate")
    if args.starting_candidate and not (args.feedback_run_manifest or args.feedback_result):
        raise ValueError("--starting-candidate requires an automatic feedback source")
    if args.feedback_result and args.feedback_run_manifest:
        raise ValueError("use exactly one feedback source")
    source_path = (
        args.starting_candidate.resolve()
        if args.starting_candidate
        else original_source_path
    )
    target_path = output_dir / "SAGenTestChecker.cpp"
    shutil.copy2(source_path, target_path)
    evidence_bundle = json.loads(args.evidence_bundle.resolve().read_text(encoding="utf-8"))
    visible_evidence, origin_gate_enabled = treatment_evidence_view(
        evidence_bundle, args.evidence_mode
    )
    metadata = json.loads(
        (case_dir / "metadata" / "candidate.json").read_text(encoding="utf-8")
    )
    commit_id = str(metadata.get("commit_id", "") or "")
    if not commit_id:
        raise ValueError("candidate metadata has no commit_id")
    frozen_source_revision = f"{commit_id}^"
    frozen_source_root = output_dir / "frozen_source"
    frozen_source_hashes = materialize_frozen_source(
        repo_root=args.evidence_dir.resolve(),
        revision=frozen_source_revision,
        patch_text=patch_path.read_text(encoding="utf-8", errors="ignore"),
        output_root=frozen_source_root,
    )
    feedback_payload = None
    feedback_summary = (
        "Frozen baseline: vulnerable-side hit present; fixed-side noise present; "
        "refinement must retain the hit and eliminate all fixed-side alerts."
    )
    if args.feedback_result:
        feedback_path = args.feedback_result.resolve()
        feedback_payload = json.loads(feedback_path.read_text(encoding="utf-8"))
        if feedback_payload.get("method") != "frozen_csa_candidate_paired_validation":
            raise ValueError("feedback result is not a frozen paired CSA validation result")
        if str(feedback_payload.get("case_id", "")) != case_dir.name:
            raise ValueError("feedback result case_id does not match the treatment case")
        if args.starting_candidate:
            expected_candidate_sha256 = str(feedback_payload.get("candidate_sha256", ""))
            if not expected_candidate_sha256 or sha256(source_path) != expected_candidate_sha256:
                raise ValueError("starting candidate does not match the paired validation result")
        vulnerable_alerts = feedback_payload.get("vulnerable_alerts")
        fixed_alerts = feedback_payload.get("fixed_alerts")
        if not feedback_payload.get("execution_valid", False):
            failure_class = "execution-invalid"
        elif not feedback_payload.get("vulnerable_hit", False):
            failure_class = "validity-loss"
        elif not feedback_payload.get("fixed_silent", False):
            failure_class = "residual-fixed-noise"
        else:
            failure_class = "paired-gate-passed"
        feedback_summary = (
            "Frozen automatic feedback from the preceding candidate: "
            f"failure_class={failure_class}; vulnerable_alerts={vulnerable_alerts}; "
            f"fixed_alerts={fixed_alerts}; pds={bool(feedback_payload.get('pds', False))}. "
            "For validity loss, restore a real use/dereference/consumer trigger on the "
            "vulnerable path without weakening the status-success barrier. For residual "
            "fixed noise, retain the vulnerable trigger while strengthening the barrier. "
            "Do not optimize only an alert count or bind to the patch site."
        )
    elif args.feedback_run_manifest:
        feedback_path = args.feedback_run_manifest.resolve()
        feedback_payload = json.loads(feedback_path.read_text(encoding="utf-8"))
        expected_method = (
            "semweaver_provenance_gated_csa_treatment"
            if origin_gate_enabled else "semweaver_no_internal_evidence_ablation"
        )
        if feedback_payload.get("method") != expected_method:
            raise ValueError("feedback manifest method differs from the selected evidence mode")
        if str(feedback_payload.get("case_id", "")) != case_dir.name:
            raise ValueError("feedback manifest case_id does not match the treatment case")
        if feedback_payload.get("success", False):
            raise ValueError("repair continuation requires a failed treatment run")
        expected_candidate_sha256 = str(feedback_payload.get("candidate_sha256", ""))
        if not expected_candidate_sha256 or sha256(source_path) != expected_candidate_sha256:
            raise ValueError("starting candidate does not match the failed run manifest")
        events_path = feedback_path.parent / "run_events.jsonl"
        failed_events = []
        for line in events_path.read_text(encoding="utf-8").splitlines():
            event = json.loads(line)
            if event.get("event") == "tool_result" and not event.get("success", False):
                failed_events.append(event)
        if failed_events:
            last_failure = failed_events[-1]
            failure_tool = str(last_failure.get("tool_name", "unknown") or "unknown")
            failure_summary = str(last_failure.get("summary", "") or "")
        else:
            failure_tool = str(
                feedback_payload.get("latest_failure_title", "local_gate") or "local_gate"
            )
            failure_summary = str(
                feedback_payload.get("latest_failure_text", "")
                or feedback_payload.get("error_message", "")
                or "candidate rejected before external paired validation"
            )
        feedback_summary = (
            "Frozen automatic tool feedback from the preceding candidate: "
            f"tool={failure_tool}; summary={failure_summary[:2400]} "
            "Repair only this failed gate while preserving the evidence-backed mechanism; "
            "do not replace it with a patch-site or name-only rule."
        )

    config = load_config(str(args.config.resolve()))
    config.setdefault("agent", {})["max_iterations"] = args.max_iterations
    # The frozen matched treatment must never change response format after a
    # transient provider error: JSON-object mode is part of its model contract.
    config["agent"]["require_json_mode"] = True
    # Keep transport attempts visible in run_events rather than allowing the
    # SDK to perform an unlogged second retry layer.
    config.setdefault("llm", {}).setdefault("generation", {})["max_retries"] = 0
    config.setdefault("llm", {}).setdefault("refine", {})["max_tokens"] = args.max_tokens
    config.setdefault("refine", {})["max_rounds"] = 1
    config["refine"]["inline_semantic_validation"] = False
    provenance_gate = config.setdefault("quality_gates", {}).setdefault(
        "evidence_provenance", {}
    )
    provenance_gate["enabled"] = origin_gate_enabled
    provenance_gate["required_origins"] = ["analyzer_internal"]
    provenance_gate["minimum_records"] = 1

    registry = build_tool_registry(
        config=config,
        options=ToolProviderOptions(
            analyzer="csa",
            include_knowledge=False,
            include_lsp=True,
            include_semantic=False,
            include_codeql=False,
            include_artifact_review=True,
            include_analyzer_selector=False,
            include_patch_analysis=False,
            include_project_analyzer=False,
            silent=True,
        ),
    )
    events = []

    def progress(payload):
        event = dict(payload or {})
        event.setdefault("timestamp", time.time())
        events.append(event)
        with (output_dir / "run_events.jsonl").open("a", encoding="utf-8") as handle:
            handle.write(json.dumps(event, ensure_ascii=False) + "\n")

    agent = LangChainRefinementAgent(
        config=config,
        tool_registry=registry,
        analyzer="csa",
        progress_callback=progress,
    )
    started = time.time()
    try:
        result = agent.run(
            RefinementRequest(
                analyzer="csa",
                patch_path=str(patch_path),
                work_dir=str(output_dir),
                target_path=str(target_path),
                source_path=str(source_path),
                validate_path="",
                evidence_dir=str(frozen_source_root),
                evidence_bundle_raw=visible_evidence,
                baseline_validation_summary=feedback_summary,
                checker_name="SAGenTestChecker",
                max_iterations=args.max_iterations,
                require_change=True,
                preload_internal_evidence=origin_gate_enabled,
                disable_evidence_requests=True,
                compile_after_each_edit=True,
            )
        )
    except ModelRateLimitExhausted as exc:
        exchange_path = output_dir / "llm_exchanges.jsonl"
        interruption = {
            "schema_version": 1,
            "status": "infrastructure_interrupted",
            "reason": "model_rate_limit_exhausted",
            "case_id": case_dir.name,
            "phase": exc.phase,
            "same_format_attempts": exc.attempts,
            "model": config["llm"]["primary_model"],
            "wire_api": config["llm"].get("wire_api", ""),
            "json_object_mode_required": True,
            "llm_exchange_log_sha256": sha256(exchange_path) if exchange_path.exists() else "",
            "event_count": len(events),
            "elapsed_seconds": time.time() - started,
        }
        (output_dir / "INFRASTRUCTURE_INTERRUPTION.json").write_text(
            json.dumps(interruption, indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8",
        )
        print(json.dumps(interruption, indent=2, ensure_ascii=False), file=sys.stderr)
        return 75
    original_checker_sha256 = sha256(original_source_path)
    starting_checker_sha256 = sha256(source_path)
    candidate_sha256 = sha256(target_path)
    candidate_changed = candidate_sha256 != starting_checker_sha256
    treatment_ready = bool(result.success and candidate_changed)
    llm_exchange_path = output_dir / "llm_exchanges.jsonl"
    payload = {
        "schema_version": 1,
        "method": (
            "semweaver_provenance_gated_csa_treatment"
            if origin_gate_enabled else "semweaver_no_internal_evidence_ablation"
        ),
        "evidence_mode": args.evidence_mode,
        "origin_gate_enabled": origin_gate_enabled,
        "case_id": case_dir.name,
        "model": config["llm"]["primary_model"],
        "provider": config["llm"]["provider"],
        "wire_api": config["llm"].get("wire_api", ""),
        "reasoning_effort": config["llm"].get("reasoning_effort", ""),
        "max_iterations": args.max_iterations,
        "max_tokens_per_call": args.max_tokens,
        "request_timeout_seconds": config["llm"].get("generation", {}).get("timeout"),
        "wrapper_max_retries": config["llm"].get("generation", {}).get("max_retries"),
        "inline_semantic_validation": False,
        "external_paired_validation_required": True,
        "internal_evidence_preloaded": origin_gate_enabled,
        "evidence_requests_disabled": True,
        "compile_after_each_edit": True,
        "llm_exchange_log": (
            str(llm_exchange_path) if llm_exchange_path.exists() else ""
        ),
        "llm_exchange_log_sha256": (
            sha256(llm_exchange_path) if llm_exchange_path.exists() else ""
        ),
        "preloaded_internal_evidence_ids": [
            str(record.get("evidence_id", "") or "")
            for record in list(visible_evidence.get("records", []) or [])
            if isinstance(record, dict)
            and str((record.get("provenance", {}) or {}).get("origin", "") or "")
            == "analyzer_internal"
        ],
        "patch_sha256": sha256(patch_path),
        "original_checker_sha256": original_checker_sha256,
        "starting_checker_sha256": starting_checker_sha256,
        "evidence_bundle_sha256": sha256(args.evidence_bundle.resolve()),
        "visible_evidence_sha256": hashlib.sha256(
            json.dumps(visible_evidence, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
        ).hexdigest(),
        "visible_evidence_record_count": len(list(visible_evidence.get("records", []) or [])),
        "frozen_source_revision": frozen_source_revision,
        "frozen_source_hashes": frozen_source_hashes,
        "feedback_result_sha256": (
            sha256(args.feedback_result.resolve()) if args.feedback_result else None
        ),
        "feedback_run_manifest_sha256": (
            sha256(args.feedback_run_manifest.resolve())
            if args.feedback_run_manifest
            else None
        ),
        "feedback_candidate_sha256": (
            feedback_payload.get("candidate_sha256") if feedback_payload else None
        ),
        "feedback_kind": (
            "paired_validation"
            if args.feedback_result
            else ("failed_tool" if args.feedback_run_manifest else None)
        ),
        "candidate_sha256": candidate_sha256,
        "candidate_changed": candidate_changed,
        "success": treatment_ready,
        "agent_success": result.success,
        "iterations": result.iterations,
        "compile_attempts": result.compile_attempts,
        "error_message": result.error_message,
        "final_message": result.final_message,
        "output_path": result.output_path,
        "elapsed_seconds": time.time() - started,
        "llm_usage": result.metadata.get("llm_usage", {}),
        "llm_usage_by_phase": result.metadata.get("llm_usage_by_phase", {}),
        "artifact_review": result.metadata.get("last_review", {}),
        "failure_type": result.metadata.get("failure_type", ""),
        "latest_failure_title": result.metadata.get("latest_failure_title", ""),
        "latest_failure_text": result.metadata.get("latest_failure_text", ""),
        "tool_history": result.metadata.get("tool_history", []),
        "event_count": len(events),
    }
    (output_dir / "RUN_MANIFEST.json").write_text(
        json.dumps(payload, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    print(json.dumps({key: value for key, value in payload.items() if key != "tool_history"}, indent=2, ensure_ascii=False))
    return 0 if treatment_ready else 2


if __name__ == "__main__":
    raise SystemExit(main())
