#!/usr/bin/env python3
"""Build a compact, path-redacted English FSE review package from frozen V7 data.

The builder refuses an existing output directory. It does not invoke a model,
modify experimental data, or access the network. Raw scientific text is copied
with only local path/account replacements, recorded in a digest crosswalk.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import re
import subprocess
import tarfile
from pathlib import Path


PROJECT = Path(__file__).resolve().parent
LLM_NATIVE = PROJECT / "LLM-Native"
ARTIFACTS = LLM_NATIVE / "artifacts" / "fse_revision"
SOURCE = LLM_NATIVE / "SemWeaver-v7"
PAPER = PROJECT / "manuscript-original-rebase"
CASES = LLM_NATIVE / "experiment_source" / "knighter" / "e1" / "cases"
RUNTIME_CASES = LLM_NATIVE / "artifacts" / "research" / "experiment" / "e1"
LINUX = LLM_NATIVE / "SemWeaver" / "artifacts" / "external" / "linux"

TEXT_SUFFIXES = {
    ".bib", ".c", ".cpp", ".csv", ".h", ".json", ".jsonl",
    ".log", ".md", ".patch", ".py", ".sh", ".sty", ".tex",
    ".txt", ".yaml", ".yml", ".toml", ".cfg", ".ini",
}
SKIP_PARTS = {
    ".git", ".venv", ".venv-dev", "__pycache__", ".pytest_cache",
    "CMakeFiles", "build", "scan-reports-0", ".latexmkrc",
}


def sha_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha(path: Path) -> str:
    return sha_bytes(path.read_bytes())


def git_head(repo: Path) -> str:
    return subprocess.run(
        ["git", "-C", str(repo), "rev-parse", "HEAD"],
        capture_output=True, text=True, check=True,
    ).stdout.strip()


def require_committed(repo: Path) -> None:
    for command in (("diff", "--quiet"), ("diff", "--cached", "--quiet")):
        result = subprocess.run(["git", "-C", str(repo), *command], check=False)
        if result.returncode != 0:
            raise RuntimeError(f"Repository has uncommitted tracked changes: {repo}")


def sanitized(data: bytes) -> tuple[bytes, list[str]]:
    # Some archived build diagnostics contain invalid UTF-8 bytes. Preserve
    # them byte-for-byte while still replacing ASCII local path segments.
    text = data.decode("utf-8", errors="surrogateescape")
    original = text
    applied = []
    replacements = (
        ("local_llm_native", str(LLM_NATIVE), "/artifact/work"),
        ("local_project", str(PROJECT), "/artifact/project"),
        ("local_home", str(PROJECT.parent), "/anonymous/home"),
        ("gateway_address", "https://model-gateway.example.invalid/v1", "https://model-gateway.example.invalid/v1"),
    )
    for label, old, new in replacements:
        if old in text:
            text = text.replace(old, new)
            applied.append(label)
    for label, pattern, replacement in (
        ("scratch_account", r"/external/upstream-study/\s\"']+", "/external/upstream-study"),
        ("generic_home_account", r"/(?:home|Users)/[^/\s\"']+", "/anonymous/home"),
    ):
        changed = re.sub(pattern, replacement, text)
        if changed != text:
            text = changed
            applied.append(label)
    return text.encode("utf-8", errors="surrogateescape"), applied if text != original else []


class Package:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.redactions: list[dict] = []

    def put(self, relative: str | Path, data: bytes, *, text: bool = True) -> None:
        relative = Path(relative)
        if relative.is_absolute() or ".." in relative.parts:
            raise ValueError(f"Unsafe package path: {relative}")
        target = self.root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        rewritten, applied = sanitized(data) if text else (data, [])
        target.write_bytes(rewritten)
        if applied:
            self.redactions.append({
                "path": relative.as_posix(),
                "original_sha256": sha_bytes(data),
                "redacted_sha256": sha_bytes(rewritten),
                "replacements": applied,
            })

    def copy(self, source: Path, relative: str | Path, *, text: bool = True) -> None:
        if not source.is_file():
            raise FileNotFoundError(source)
        self.put(relative, source.read_bytes(), text=text)

    def copy_tree(self, source: Path, prefix: str | Path) -> None:
        for path in sorted(source.rglob("*")):
            if not path.is_file():
                continue
            relative = path.relative_to(source)
            if any(part in SKIP_PARTS for part in relative.parts):
                continue
            if path.suffix.lower() not in TEXT_SUFFIXES:
                continue
            self.copy(path, Path(prefix) / relative)

    def export_git(self, repo: Path, prefix: str | Path) -> None:
        archive = subprocess.run(
            ["git", "-C", str(repo), "archive", "HEAD"],
            capture_output=True, check=True,
        ).stdout
        with tarfile.open(fileobj=io.BytesIO(archive), mode="r:") as handle:
            for member in handle:
                if not member.isfile():
                    continue
                relative = Path(member.name)
                if relative.is_absolute() or ".." in relative.parts:
                    raise ValueError(f"Unsafe archive entry: {member.name}")
                stream = handle.extractfile(member)
                assert stream is not None
                self.put(Path(prefix) / relative, stream.read(), text=relative.suffix.lower() in TEXT_SUFFIXES)


def copy_evidence(package: Package, case_ids: list[str]) -> None:
    source = ARTIFACTS / "semweaver_treatment_evidence_cohort_v3"
    prefix = Path("evidence") / "semweaver_treatment_evidence_cohort_v3"
    for name in ("BATCH_MANIFEST.json", "BATCH_RESULT.json", "BINDING_AUDIT.json", "PROVENANCE_AUDIT.json"):
        package.copy(source / name, prefix / name)
    for case_id in case_ids:
        for name in ("EVIDENCE_REPLAY_MANIFEST.json", "csa/evidence_bundle.json"):
            package.copy(source / case_id / name, prefix / case_id / name)
        bundle = json.loads((source / case_id / "csa" / "evidence_bundle.json").read_text(encoding="utf-8"))
        for record in bundle["records"]:
            if record.get("provenance", {}).get("origin") != "analyzer_internal":
                continue
            raw_path = str(record["semantic_payload"]["raw_output_path"])
            if "/fse_revision/" not in raw_path:
                raise ValueError(f"Unexpected internal raw path: {case_id}")
            remainder = raw_path.split("/fse_revision/", 1)[1]
            raw_source = (ARTIFACTS / remainder).resolve()
            if not raw_source.is_relative_to(ARTIFACTS.resolve()):
                raise ValueError(f"Raw path escapes artifact root: {case_id}")
            package.copy(raw_source, Path("evidence") / remainder)


def copy_source_blobs(package: Package, case_ids: list[str]) -> None:
    evidence = ARTIFACTS / "semweaver_treatment_evidence_cohort_v3"
    audit = json.loads((evidence / "BINDING_AUDIT.json").read_text(encoding="utf-8"))
    audits = {row["case_id"]: row for row in audit["rows"]}
    blobs = []
    for case_id in case_ids:
        metadata = json.loads((CASES / case_id / "metadata" / "candidate.json").read_text(encoding="utf-8"))
        revision = str(metadata["commit_id"]) + "^"
        for filename in audits[case_id]["patch_files"]:
            spec = f"{revision}:{filename}"
            data = subprocess.run(["git", "-C", str(LINUX), "show", spec], capture_output=True, check=True).stdout
            oid = subprocess.run(
                ["git", "-C", str(LINUX), "rev-parse", spec],
                capture_output=True, text=True, check=True,
            ).stdout.strip()
            relative = Path("inputs") / "vulnerable_source" / case_id / filename
            package.put(relative, data, text=False)
            blobs.append({
                "case_id": case_id, "commit_id": metadata["commit_id"],
                "revision": revision, "path": filename, "sha256": sha_bytes(data),
                "git_blob_sha1": oid, "size": len(data),
            })
    package.put("inputs/VULNERABLE_SOURCE_BLOBS.json", (json.dumps({
        "schema_version": 1, "blob_count": len(blobs), "blobs": blobs,
    }, indent=2) + "\n").encode())


def copy_repeats(package: Package) -> None:
    repeats = ARTIFACTS / "repeats_v7"
    for name in ("PROTOCOL.json", "REPEAT_PLAN.json", "REPEAT_MANIFEST_BINDING.json", "ORACLE_AMENDMENT.md", "V7_REPEAT_SUMMARY.json"):
        package.copy(repeats / name, Path("evidence/repeats_v7") / name)
    for replicate in ("rep01", "rep02", "rep03"):
        source = repeats / replicate
        target = Path("evidence/repeats_v7") / replicate
        if replicate == "rep01":
            package.copy_tree(ARTIFACTS / "repeats_v6" / "rep01" / "knighter", target / "knighter")
            package.copy_tree(ARTIFACTS / "repeats_v6" / "rep01" / "knighter_strict", target / "knighter_strict_original")
        else:
            package.copy_tree(source / "knighter", target / "knighter")
            package.copy_tree(source / "knighter_strict", target / "knighter_strict_original") if (source / "knighter_strict").is_dir() else None
        for arm in ("knighter_strict_pds", "native", "no_internal"):
            package.copy_tree(source / arm, target / arm)
        for path in sorted(source.iterdir()):
            if path.is_file() and path.suffix.lower() in TEXT_SUFFIXES:
                package.copy(path, target / path.name)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    require_committed(SOURCE)
    require_committed(PAPER)
    expansion_result = json.loads((ARTIFACTS / "expansion_validation_v2" / "VALIDATION_RESULT.json").read_text(encoding="utf-8"))
    if expansion_result.get("completed_records") != expansion_result.get("requested_cases") or expansion_result.get("requested_cases") != 10:
        raise RuntimeError("Ten-case expansion validation is incomplete")
    repeated = json.loads((ARTIFACTS / "repeats_v7" / "V7_REPEAT_SUMMARY.json").read_text(encoding="utf-8"))
    if repeated.get("replicate_count") != 3 or repeated.get("distinct_subjects") != 12:
        raise RuntimeError("Repeated-decode summary is incomplete")
    output = args.output_dir.resolve()
    if output.exists():
        raise SystemExit(f"Refusing to overwrite: {output}")
    output.mkdir(parents=True)
    package = Package(output)
    package.export_git(SOURCE, "source/SemWeaver")
    package.export_git(PAPER, "source/manuscript")
    package.copy(PAPER / "main.pdf", "paper/main.pdf", text=False)
    for name in (
        "summarize_v7_repeats.py", "compare_v7_paired.py", "compare_v7_evidence_arms.py",
        "recompute_fse_portfolio.py", "audit_e2_legacy.py", "validate_knighter_expansion.py",
        "build_v7_review_package.py", "verify_v7_review_package.py",
    ):
        package.copy(PROJECT / name, Path("scripts") / name)
    manifest = json.loads((ARTIFACTS / "semweaver_treatment_evidence_cohort_v3" / "BATCH_MANIFEST.json").read_text(encoding="utf-8"))
    case_ids = [str(item["case_id"]) for item in manifest["cases"]]
    if len(case_ids) != 12 or len(set(case_ids)) != 12:
        raise ValueError("Invalid frozen subject list")
    for case_id in case_ids:
        package.copy_tree(CASES / case_id, Path("inputs/e1/cases") / case_id)
        runtime_metadata = RUNTIME_CASES / case_id / "metadata" / "candidate.json"
        if runtime_metadata.is_file() and sha(runtime_metadata) != sha(CASES / case_id / "metadata" / "candidate.json"):
            package.copy(runtime_metadata, Path("inputs/e1/runtime_metadata") / case_id / "candidate.json")
    package.copy_tree(LLM_NATIVE / "experiment_source" / "knighter" / "e1" / "manifests", "inputs/e1/manifests")
    package.copy(LLM_NATIVE / "experiment_source" / "knighter" / "e1" / "results" / "screening" / "e2_screen1.csv", "inputs/e1/e2_screen1.csv")
    copy_source_blobs(package, case_ids)
    copy_evidence(package, case_ids)
    copy_repeats(package)
    for name in (
        "portfolio_original_v1", "e2_legacy_audit_v1", "expansion_candidates_v1",
        "expansion_materialized_v1", "expansion_validation_v2",
    ):
        package.copy_tree(ARTIFACTS / name, Path("evidence") / name)
    package.copy_tree(ARTIFACTS / "repeats_v7" / "case19_patch_local_probe", "evidence/repeats_v7/case19_patch_local_probe")
    package.copy(PROJECT / "ARTIFACT_RELEASE_README.md", "README.md")

    redaction_path = output / "REDACTIONS.json"
    redaction_path.write_text(json.dumps({
        "schema_version": 1,
        "policy": "Only local account paths and the private gateway address are replaced; original digests remain bound.",
        "file_count": len(package.redactions),
        "files": package.redactions,
    }, indent=2) + "\n", encoding="utf-8")
    files = [
        {"path": path.relative_to(output).as_posix(), "sha256": sha(path), "size": path.stat().st_size}
        for path in sorted(output.rglob("*"))
        if path.is_file() and path.name != "ARTIFACT_MANIFEST.json"
    ]
    manifest_path = output / "ARTIFACT_MANIFEST.json"
    manifest_path.write_text(json.dumps({
        "schema_version": 1,
        "semweaver_revision": git_head(SOURCE),
        "manuscript_revision": git_head(PAPER),
        "case_count": len(case_ids),
        "file_count": len(files),
        "files": files,
    }, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"output": str(output), "files": len(files), "redactions": len(package.redactions)}, indent=2))


if __name__ == "__main__":
    main()
