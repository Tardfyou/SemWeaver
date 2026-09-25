#!/usr/bin/env python3
"""Audit strict CSA internal-evidence binding for a frozen cohort replay."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(PROJECT_ROOT))

from src.evidence.collectors.artifact_extractor import ProjectArtifactExtractor


REQUIRED_INTERNAL_FIELDS = (
    "interface",
    "output_schema",
    "raw_output_path",
    "raw_output_sha256",
    "command_sha256",
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def patch_scope(patch_path: Path) -> tuple[set[str], dict[str, set[str]]]:
    extractor = ProjectArtifactExtractor()
    files = set()
    functions: dict[str, set[str]] = {}
    for patch_file in extractor.parse_patch(str(patch_path)):
        file_name = str(patch_file.get("old_path") or patch_file.get("new_path") or "")
        if not file_name:
            continue
        files.add(file_name)
        for hunk in list(patch_file.get("hunks", []) or []):
            hint = extractor.hunk_function_hint(hunk)
            if hint:
                functions.setdefault(file_name, set()).add(hint)
    return files, functions


def resolve_recorded_path(raw_path: str, workspace_root: Path) -> Path:
    value = str(raw_path or "")
    if value.startswith("/work/"):
        return workspace_root / value.removeprefix("/work/")
    return Path(value).expanduser()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--batch-root", required=True, type=Path)
    parser.add_argument("--cases-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    batch_root = args.batch_root.resolve()
    cases_root = args.cases_root.resolve()
    workspace_root = cases_root.parents[3]
    batch_result_path = batch_root / "BATCH_RESULT.json"
    batch_manifest_path = batch_root / "BATCH_MANIFEST.json"
    batch_result = json.loads(batch_result_path.read_text(encoding="utf-8"))
    rows = []
    for batch_row in list(batch_result.get("rows", []) or []):
        case_id = str(batch_row.get("case_id", "") or "")
        case_root = batch_root / case_id
        case_manifest_path = case_root / "EVIDENCE_REPLAY_MANIFEST.json"
        bundle_path = case_root / "csa" / "evidence_bundle.json"
        patch_path = cases_root / case_id / "patches" / "commit.patch"
        errors = []
        if sha256(case_manifest_path) != str(batch_row.get("result_sha256", "")):
            errors.append("batch row result hash mismatch")
        case_manifest = json.loads(case_manifest_path.read_text(encoding="utf-8"))
        if sha256(bundle_path) != str(case_manifest.get("evidence_bundle_sha256", "")):
            errors.append("case evidence bundle hash mismatch")
        patch_files, patch_functions = patch_scope(patch_path)
        bundle = json.loads(bundle_path.read_text(encoding="utf-8"))
        internal = [
            record
            for record in list(bundle.get("records", []) or [])
            if str((record.get("provenance", {}) or {}).get("origin", ""))
            == "analyzer_internal"
        ]
        if not internal:
            errors.append("no analyzer-internal record")
        bound_records = 0
        for record in internal:
            evidence_id = str(record.get("evidence_id", "") or "<unknown>")
            scope = record.get("scope", {}) or {}
            payload = record.get("semantic_payload", {}) or {}
            file_name = str(scope.get("file", "") or "")
            function_name = str(scope.get("function", "") or "")
            if file_name not in patch_files:
                errors.append(f"{evidence_id}: file outside patch scope")
            expected_functions = patch_functions.get(file_name, set())
            if not function_name:
                errors.append(f"{evidence_id}: missing function binding")
            elif expected_functions and function_name not in expected_functions:
                errors.append(f"{evidence_id}: function outside hunk scope")
            else:
                bound_records += 1
            for field in REQUIRED_INTERNAL_FIELDS:
                if not payload.get(field):
                    errors.append(f"{evidence_id}: missing {field}")
            raw_path = resolve_recorded_path(
                str(payload.get("raw_output_path", "") or ""), workspace_root
            )
            if not raw_path.is_file():
                errors.append(f"{evidence_id}: raw output missing")
            elif sha256(raw_path) != str(payload.get("raw_output_sha256", "")):
                errors.append(f"{evidence_id}: raw output hash mismatch")
        rows.append({
            "case_id": case_id,
            "internal_records": len(internal),
            "bound_internal_records": bound_records,
            "patch_files": sorted(patch_files),
            "patch_functions": {
                key: sorted(value) for key, value in sorted(patch_functions.items())
            },
            "passed": not errors,
            "errors": errors,
        })

    payload = {
        "schema_version": 1,
        "method": "strict_csa_internal_evidence_binding_audit",
        "batch_manifest_sha256": sha256(batch_manifest_path),
        "batch_result_sha256": sha256(batch_result_path),
        "subjects": len(rows),
        "passed_subjects": sum(row["passed"] for row in rows),
        "internal_records": sum(row["internal_records"] for row in rows),
        "bound_internal_records": sum(row["bound_internal_records"] for row in rows),
        "passed": bool(rows) and all(row["passed"] for row in rows),
        "rows": rows,
    }
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        json.dumps(payload, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    print(json.dumps(payload, indent=2, ensure_ascii=False))
    return 0 if payload["passed"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
