#!/usr/bin/env python3
"""Automatic CodeQL target-hit recovery probe with independent paired follow-up.

Development-only: starts from an archived generator query, copies it unchanged,
then allows only the model-driven SemWeaver agent to edit the working copy.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import sys
import time
from pathlib import Path


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def patch_anchored_source_window(patch: Path, source_root: Path, radius: int) -> str:
    """Deterministically expose vulnerable source near every frozen patch hunk."""
    if radius <= 0:
        return ""
    source_root = source_root.resolve()
    source_lines: dict[Path, set[int]] = {}
    current: Path | None = None
    for line in patch.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("diff --git a/"):
            relative = line.split(" b/", 1)[0][len("diff --git a/"):]
            candidate = (source_root / relative).resolve()
            current = candidate if candidate.is_relative_to(source_root) and candidate.is_file() else None
        match = re.match(r"@@ -(\d+)(?:,\d+)? \+\d+(?:,\d+)? @@", line)
        if match and current is not None:
            source_lines.setdefault(current, set()).add(int(match.group(1)))
    chunks = []
    for path, anchors in sorted(source_lines.items()):
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        selected = set()
        for anchor in anchors:
            selected.update(range(max(1, anchor - radius), min(len(lines), anchor + radius) + 1))
        rendered = "\n".join(f"{number}: {lines[number - 1]}" for number in sorted(selected))
        chunks.append(f"VULNERABLE SOURCE {path.relative_to(source_root)} (patch-hunk windows):\n{rendered}")
    return "\n\n".join(chunks)[:30000]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--semweaver-root", required=True, type=Path)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--initial-query", required=True, type=Path)
    parser.add_argument("--patch", required=True, type=Path)
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--vulnerable-db", required=True, type=Path)
    parser.add_argument("--fixed-db", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--feedback-result", type=Path,
                        help="Independent paired replay of the exact input query")
    parser.add_argument("--patch-window-radius", type=int, default=0,
                        help="Include deterministic vulnerable source context around patch hunks")
    parser.add_argument("--max-iterations", type=int, default=4)
    parser.add_argument("--max-model-calls", type=int, default=4)
    parser.add_argument("--max-tokens", type=int, default=16384)
    args = parser.parse_args()
    root = args.semweaver_root.resolve()
    sys.path.insert(0, str(root))
    from src.refine import LangChainRefinementAgent, RefinementRequest  # noqa: E402
    from src.refine.agent import ModelRateLimitExhausted  # noqa: E402
    from src.tools import ToolProviderOptions, build_tool_registry  # noqa: E402
    from src.utils import load_config  # noqa: E402

    initial = args.initial_query.resolve()
    patch = args.patch.resolve()
    source = args.source_root.resolve()
    vuln_db = args.vulnerable_db.resolve()
    fixed_db = args.fixed_db.resolve()
    pack = initial.parent / "qlpack.yml"
    for required in (initial, patch, pack, vuln_db / "codeql-database.yml", fixed_db / "codeql-database.yml"):
        if not required.is_file():
            raise FileNotFoundError(required)
    feedback_path = args.feedback_result.resolve() if args.feedback_result else None
    feedback = None
    if feedback_path:
        feedback = json.loads(feedback_path.read_text(encoding="utf-8"))
        if feedback.get("query_sha256") != sha256(initial):
            raise ValueError("Paired input feedback does not bind to the input query")
        if feedback.get("execution_valid", False) and feedback.get("pds", False):
            raise ValueError("Input query already has the desired paired outcome")
    source_window = patch_anchored_source_window(patch, source, args.patch_window_radius)
    output = args.output_dir.resolve()
    if output.exists():
        raise FileExistsError(output)
    output.mkdir(parents=True)
    query = output / initial.name
    shutil.copyfile(initial, query)
    # The archived query's package declares an unquoted YAML wildcard.  It is
    # package metadata, not checker logic, and CodeQL rejects it before the
    # model-produced query can be parsed.  Record the exact normalization so
    # this setup repair cannot be mistaken for an automatic query improvement.
    pack_text = pack.read_text(encoding="utf-8")
    invalid_dependency = "  codeql/cpp-all: *\n"
    normalized_dependency = '  codeql/cpp-all: "*"\n'
    if pack_text.count(invalid_dependency) > 1:
        raise ValueError("Ambiguous qlpack wildcard dependency")
    normalized_pack = pack_text.replace(invalid_dependency, normalized_dependency)
    (output / "qlpack.yml").write_text(normalized_pack, encoding="utf-8")
    frozen = {
        "schema_version": 1,
        "method": "e2_codeql_automatic_development_probe",
        "objective": "target_hit_recovery",
        "initial_query_sha256": sha256(initial),
        "working_initial_sha256": sha256(query),
        "qlpack_source_sha256": sha256(pack),
        "qlpack_runtime_sha256": sha256(output / "qlpack.yml"),
        "qlpack_runtime_fix": "quote_unquoted_cpp_all_wildcard" if normalized_pack != pack_text else "none",
        "patch_sha256": sha256(patch),
        "source_root": str(source),
        "vulnerable_database": str(vuln_db),
        "fixed_database": str(fixed_db),
        "model_call_cap": args.max_model_calls,
        "max_tokens_per_call": args.max_tokens,
        "external_paired_validation_required": True,
        "input_feedback_result_sha256": sha256(feedback_path) if feedback_path else "",
        "source_window_radius": args.patch_window_radius,
        "source_window_sha256": hashlib.sha256(source_window.encode("utf-8")).hexdigest(),
        "inference_boundary": "development-only; does not establish cross-backend effect",
    }
    (output / "INPUT_MANIFEST.json").write_text(json.dumps(frozen, indent=2) + "\n", encoding="utf-8")
    config = load_config(str(args.config.resolve()))
    config.setdefault("agent", {})["max_iterations"] = args.max_iterations
    config["agent"]["max_model_calls"] = args.max_model_calls
    config["agent"]["require_json_mode"] = True
    config["agent"]["refine_decision_temperature"] = 0.0
    config["agent"]["refine_repair_temperature"] = 0.0
    config.setdefault("llm", {}).setdefault("generation", {})["max_retries"] = 0
    config["llm"].setdefault("refine", {})["max_tokens"] = args.max_tokens
    config.setdefault("quality_gates", {}).setdefault("evidence_provenance", {})["enabled"] = False
    config.setdefault("codeql", {})["database_path"] = str(vuln_db)
    config["codeql"]["timeout"] = 300
    config["codeql"]["max_memory_mb"] = 4096
    config.setdefault("refine", {})["max_rounds"] = 1
    registry = build_tool_registry(
        config=config,
        options=ToolProviderOptions(
            analyzer="codeql", include_knowledge=False, include_lsp=False,
            include_semantic=False, include_codeql=True,
            include_artifact_review=True, include_analyzer_selector=False,
            include_patch_analysis=False, include_project_analyzer=False,
            silent=True,
        ),
    )
    events = []
    event_path = output / "run_events.jsonl"

    def progress(payload):
        item = dict(payload or {})
        events.append(item)
        with event_path.open("a", encoding="utf-8") as handle:
            handle.write(json.dumps(item, ensure_ascii=False) + "\n")

    agent = LangChainRefinementAgent(
        config=config, tool_registry=registry, analyzer="codeql",
        progress_callback=progress,
    )
    started = time.monotonic()
    if feedback is None:
        validation_summary = (
            "Independent CodeQL no-build paired replay of the archived generated query: "
            "vulnerable_rows=0, fixed_rows=0. Recover a real vulnerable target hit "
            "without introducing fixed-side rows; final paired replay is external."
        )
    elif feedback.get("execution_valid", False):
        vulnerable_rows = int(feedback["vulnerable_rows"])
        fixed_rows = int(feedback["fixed_rows"])
        validation_summary = (
            "Independent CodeQL paired replay of this exact input query: "
            f"vulnerable_rows={vulnerable_rows}, fixed_rows={fixed_rows}. "
        )
        if vulnerable_rows > 0 and fixed_rows > 0:
            validation_summary += (
                "The target already fires, but the fixed revision remains noisy. "
                "Preserve the real target trigger while tightening the semantic "
                "relation among deletion, alias, use, and invalidation barrier; "
                "require a positive AST/data-flow/alias join from the deleted "
                "value to the later use. Merely mentioning independent variables "
                "in the same function or joining them only inside a negated "
                "guard creates a Cartesian-product false positive. "
                "do not substitute variable-name or patch-line fingerprints. "
            )
        else:
            validation_summary += (
                "This automatic candidate still misses the vulnerable target. "
                "Recover a real vulnerable target hit without introducing "
                "fixed-side rows. "
            )
        for side in ("vulnerable", "fixed"):
            samples = []
            for row in list((feedback.get("sides", {}).get(side, {}) or {}).get("sample_rows", []) or [])[:5]:
                entity = row[0] if row and isinstance(row[0], dict) else {}
                location = entity.get("url", {}) or {}
                samples.append({
                    "label": str(entity.get("label", ""))[:80],
                    "line": location.get("startLine"),
                    "message": str(row[-1])[:200] if row else "",
                })
            if samples:
                validation_summary += f"\n{side} analyzer-output sample rows: {json.dumps(samples, ensure_ascii=False)}"
        validation_summary += "Final paired replay is external."
    else:
        reason = str((feedback.get("sides", {}).get("vulnerable", {}) or {}).get("reason", "execution_error"))
        validation_summary = (
            "Independent CodeQL replay of this exact input query did not complete: "
            f"vulnerable-side reason={reason}. This is unscored, not a negative "
            "detection result. Repair the query's expensive or invalid relation "
            "without merely dropping the target mechanism or hard-coding the "
            "patch line; the final paired replay is external."
        )
    if source_window:
        validation_summary += "\n\n" + source_window
    try:
        result = agent.run(RefinementRequest(
            analyzer="codeql",
            patch_path=str(patch),
            work_dir=str(output),
            target_path=str(query),
            source_path=str(initial),
            validate_path=str(source),
            evidence_dir=str(source),
            evidence_bundle_raw={"records": [], "missing_evidence": [], "collected_analyzers": []},
            baseline_validation_summary=validation_summary,
            checker_name=query.stem,
            objective="target_hit_recovery",
            max_iterations=args.max_iterations,
            require_change=True,
            disable_evidence_requests=True,
        ))
    except ModelRateLimitExhausted as exc:
        (output / "INFRASTRUCTURE_INTERRUPTION.json").write_text(json.dumps({
            "reason": "model_rate_limit_exhausted", "phase": exc.phase,
            "attempts": exc.attempts, "event_count": len(events),
        }, indent=2) + "\n", encoding="utf-8")
        return 75
    payload = {
        **frozen,
        "success": bool(result.success and sha256(query) != frozen["initial_query_sha256"]),
        "agent_success": bool(result.success),
        "query_sha256": sha256(query),
        "iterations": result.iterations,
        "error_message": result.error_message,
        "final_message": result.final_message,
        "llm_usage": result.metadata.get("llm_usage", {}),
        "elapsed_seconds": time.monotonic() - started,
        "event_count": len(events),
        "effect_status": "unvalidated_pending_external_pair",
    }
    (output / "RUN_MANIFEST.json").write_text(json.dumps(payload, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps({key: payload[key] for key in (
        "success", "agent_success", "query_sha256", "iterations",
        "error_message", "llm_usage", "elapsed_seconds"
    )}, indent=2, ensure_ascii=False))
    return 0 if payload["success"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
