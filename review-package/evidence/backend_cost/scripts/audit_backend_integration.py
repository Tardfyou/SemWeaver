#!/usr/bin/env python3
"""Hash-bound implementation footprint, not a retrospective labor estimate."""
import argparse
import ast
import hashlib
import json
import subprocess
from pathlib import Path

SOURCE_FILES = {
    "backend_specific": [
        "src/core/codeql_analyzer.py", "src/evidence/collectors/codeql_flow.py",
        "src/validation/codeql_support.py", "src/tools/codeql_analyze.py",
    ],
    "shared_with_backend_branches": [
        "src/core/analyzer_base.py", "src/evidence/collectors/artifact_extractor.py",
        "src/validation/semantic_validator.py", "src/validation/unified_validator.py",
        "src/tools/__init__.py", "src/refine/agent.py",
    ],
    "shared_schema_and_provenance": [
        "src/core/evidence_schema.py", "src/evidence/collectors/base.py",
    ],
    "backend_tests": ["tests/test_codeql_pack_preservation.py"],
}
DRIVERS = ["run_e2_codeql_development.py", "run_e2_codeql_paired_loop.py", "validate_e2_codeql_pair.py", "test_e2_codeql_paired_loop.py"]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def describe(path, name, category):
    text = path.read_text()
    tree = ast.parse(text)
    return {
        "path": name, "category": category, "sha256": sha(path),
        "physical_lines": len(text.splitlines()),
        "nonblank_lines": sum(bool(line.strip()) for line in text.splitlines()),
        "functions": sum(isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) for node in ast.walk(tree)),
        "classes": sum(isinstance(node, ast.ClassDef) for node in ast.walk(tree)),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    root = args.source_root.resolve()
    output = args.output_dir.resolve()
    if output.exists():
        raise FileExistsError(output)
    revision = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"], check=True, capture_output=True, text=True).stdout.strip()
    rows = []
    for category, paths in SOURCE_FILES.items():
        for name in paths:
            path = root / name
            committed = subprocess.run(["git", "-C", str(root), "show", f"{revision}:{name}"], check=True, capture_output=True).stdout
            if hashlib.sha256(committed).hexdigest() != sha(path):
                raise RuntimeError(f"Source differs from pinned commit: {name}")
            rows.append(describe(path, name, category))
    for name in DRIVERS:
        rows.append(describe(Path(__file__).resolve().parent / name, name, "external_e2_protocol_and_tests"))
    summary = {}
    for category in sorted({r["category"] for r in rows}):
        selected = [r for r in rows if r["category"] == category]
        summary[category] = {"files": len(selected), "physical_lines": sum(r["physical_lines"] for r in selected)}
    output.mkdir(parents=True)
    payload = {
        "schema_version": 1, "source_revision": revision, "rows": rows, "summary": summary,
        "driver_sha256": sha(Path(__file__)),
        "scope": "Explicitly enumerated implementation surface of existing CodeQL integration; whole-file physical lines include comments and generation code. Shared files are not counted as newly added CodeQL code. This is neither a complete dependency closure nor an estimate of marginal LOC, engineering hours, or effort for an unseen backend.",
        "historical_developer_hours": None,
        "historical_hours_reason": "No contemporaneous development time log was retained; Git timestamps and replay latency cannot reconstruct developer effort.",
    }
    (output / "RESULT.json").write_text(json.dumps(payload, indent=2) + "\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
