#!/usr/bin/env python3
"""Build an English, hash-bound 39-case FSE review package without network access.

This builder deliberately refuses incomplete GLM repeat batches, dirty source
commits, and an existing output directory. It copies evidence, not Linux
worktrees, CodeQL databases, virtual environments, or compiler caches.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import re
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path


PROJECT = Path(__file__).resolve().parent
WORK = PROJECT / "LLM-Native"
DATA = WORK / "artifacts" / "fse_revision"
SOURCE = WORK / "SemWeaver-v22"
AUDITOR = WORK / "SemWeaver-v20"
PAPER = PROJECT / "manuscript-original-rebase"
SOURCE_EXPECTED = "a5cede46bdd79d91ec5c8874e95c8a0b0206d99b"
AUDITOR_EXPECTED = "bbc0f4320caa4df50b9f87bd3848dd3a1d498d73"
HISTORICAL_IMPLS = {
    "motivating_quality_v23": "f3c57bb2874ad38597ac446da6c516e564c0e1a2",
    "gpt_e4_v10": "aaea15652bc55d6b0b442f2a85bc7852365b306c",
    "e3_matched_v13": "364d8cd53ddaed32492369b1583e22af1716099d",
    "glm_precision_v11": "c6aff4c10809200f24c36a86738243a8e1f5cbc0",
    "glm_anthropic_v19": "0788609a5048d8480244bc56aba5fca305d0df3b",
    "glm_anthropic_v21": "3d5e080cf85a1fa3001a827e4c58cfbb8e87e9b7",
}
MAX_FILE_BYTES = 95 * 1024 * 1024
TEXT_SUFFIXES = {
    ".bib", ".c", ".cpp", ".csv", ".h", ".json", ".jsonl", ".log",
    ".md", ".patch", ".py", ".ql", ".sh", ".sty", ".tex", ".txt",
    ".text", ".diff", ".yaml", ".yml", ".toml", ".cfg", ".ini",
    ".html", ".plist", ".xml", ".sarif", ".css", ".js",
}
SKIP_PARTS = {
    ".git", ".venv", ".venv-dev", "__pycache__", ".pytest_cache",
    "CMakeFiles", "cmake-build", "build", "scan-reports-0", "scan-reports",
    ".cache", "node_modules",
}
SUMMARY_DIRS = (
    "all39_knighter_system_v10",
    "e1_matched14_gpt6luna_high_native_comparison_v20",
    "e1_matched14_gpt6luna_high_no_internal_comparison_v20",
    "e3_all39_gpt6luna_high_native_v13_portfolio",
    "e3_all39_gpt6luna_high_no_internal_v13_portfolio",
    "e4_all39_gpt6luna_high_portfolio_v19",
    "e4_all39_glm53_mixed_wire_portfolio_v20",
    "e4_all39_glmflash_mixed_wire_portfolio_v20",
    "all39_case_table_v1",
    "matched14_case_table_v1",
    "e3_repeat12_case_table_v1",
)


def sha_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def committed_head(repo: Path) -> str:
    status = subprocess.run(
        ["git", "-C", str(repo), "status", "--porcelain", "--untracked-files=no"],
        capture_output=True, text=True, check=True,
    ).stdout
    if status.strip():
        raise RuntimeError(f"Refusing dirty tracked source: {repo}")
    return subprocess.run(
        ["git", "-C", str(repo), "rev-parse", "HEAD"],
        capture_output=True, text=True, check=True,
    ).stdout.strip()


def redact(data: bytes) -> tuple[bytes, list[str]]:
    text = data.decode("utf-8", errors="surrogateescape")
    original = text
    labels: list[str] = []
    for label, old, new in (
        ("machine_hostname", "anonymous-host", "anonymous-host"),
        ("unrelated_tool_cache_name", "codeql-", "codeql-"),
        ("local_work", str(WORK), "/artifact/work"),
        ("local_project", str(PROJECT), "/artifact/project"),
        ("local_home", str(PROJECT.parent), "/anonymous/home"),
        ("gateway_address", "https://model-gateway.example.invalid/v1", "https://model-gateway.example.invalid/v1"),
        ("gateway_host", "https://model-gateway.example.invalid", "https://model-gateway.example.invalid"),
        ("gateway_https_host", "https://model-gateway.example.invalid", "https://model-gateway.example.invalid"),
    ):
        if old in text:
            text = text.replace(old, new)
            labels.append(label)
    for label, pattern, replacement in (
        ("scratch_account", r"/external/upstream-study/\s\"']+", "/external/upstream-study"),
        ("generic_home_account", r"(?<!/anonymous)/(?:home|Users)/[^/\s\"']+", "/anonymous/home"),
    ):
        changed = re.sub(pattern, replacement, text)
        if changed != text:
            labels.append(label)
            text = changed
    return text.encode("utf-8", errors="surrogateescape"), labels if text != original else []


class Package:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.redactions: list[dict] = []

    def put(self, relative: str | Path, data: bytes, *, text: bool = True) -> None:
        relative = Path(relative)
        if relative.is_absolute() or ".." in relative.parts:
            raise ValueError(f"Unsafe package path: {relative}")
        if len(data) > MAX_FILE_BYTES:
            raise ValueError(f"Oversize package file: {relative}")
        target = self.root / relative
        if target.exists():
            raise ValueError(f"Duplicate package path: {relative}")
        target.parent.mkdir(parents=True, exist_ok=True)
        rewritten, labels = redact(data) if text else (data, [])
        target.write_bytes(rewritten)
        if labels:
            self.redactions.append({
                "path": relative.as_posix(),
                "original_sha256": sha_bytes(data),
                "redacted_sha256": sha_bytes(rewritten),
                "replacements": labels,
            })

    def copy(self, source: Path, relative: str | Path) -> None:
        if not source.is_file() or source.is_symlink():
            raise FileNotFoundError(source)
        self.put(relative, source.read_bytes(), text=source.suffix.lower() in TEXT_SUFFIXES)

    def tree(self, source: Path, relative: str | Path) -> None:
        if not source.is_dir():
            raise FileNotFoundError(source)
        for path in sorted(source.rglob("*")):
            if not path.is_file() or path.is_symlink():
                continue
            subpath = path.relative_to(source)
            if any(part in SKIP_PARTS or part.startswith("scan-reports-") for part in subpath.parts):
                continue
            if path.suffix.lower() not in TEXT_SUFFIXES:
                continue
            self.copy(path, Path(relative) / subpath)

    def git_archive(self, repo: Path, relative: str | Path, revision: str = "HEAD") -> None:
        archive = subprocess.run(
            ["git", "-C", str(repo), "archive", revision], capture_output=True, check=True,
        ).stdout
        with tarfile.open(fileobj=io.BytesIO(archive), mode="r:") as handle:
            for member in handle:
                if not member.isfile():
                    continue
                subpath = Path(member.name)
                if subpath.is_absolute() or ".." in subpath.parts:
                    raise ValueError(f"Unsafe git archive member: {member.name}")
                stream = handle.extractfile(member)
                assert stream is not None
                is_text = subpath.suffix.lower() in TEXT_SUFFIXES or subpath.name in {".gitignore", ".env.example"}
                self.put(Path(relative) / subpath, stream.read(), text=is_text)

    def finish(self, metadata: dict) -> None:
        self.put("REDACTIONS.json", (json.dumps({
            "schema_version": 1, "file_count": len(self.redactions),
            "files": self.redactions,
        }, indent=2) + "\n").encode())
        files = []
        for path in sorted(self.root.rglob("*")):
            if path.is_file() and path.relative_to(self.root).as_posix() != "ARTIFACT_MANIFEST.json":
                files.append({
                    "path": path.relative_to(self.root).as_posix(),
                    "size": path.stat().st_size,
                    "sha256": sha_file(path),
                })
        manifest = {"schema_version": 1, **metadata, "file_count": len(files), "files": files}
        self.put("ARTIFACT_MANIFEST.json", (json.dumps(manifest, indent=2) + "\n").encode())


def all_batch_hashes(value: object) -> set[str]:
    found: set[str] = set()
    if isinstance(value, dict):
        for key, child in value.items():
            if key.endswith("batch_result_sha256") and isinstance(child, str) and len(child) == 64:
                found.add(child)
            found.update(all_batch_hashes(child))
    elif isinstance(value, list):
        for child in value:
            found.update(all_batch_hashes(child))
    return found


def copy_evidence(package: Package) -> list[str]:
    source = DATA / "generate_only_evidence_all39_v8_r5"
    prefix = Path("evidence/generate_only_evidence_all39_v8_r5")
    for name in ("BATCH_MANIFEST.json", "BATCH_RESULT.json", "STRICT_BINDING_AUDIT.json"):
        package.copy(source / name, prefix / name)
    manifest = load(source / "BATCH_MANIFEST.json")
    result = load(source / "BATCH_RESULT.json")
    if manifest["case_count"] != result["requested_cases"] or result["completed_records"] != 39:
        raise RuntimeError("Evidence replay is not complete for 39 cases")
    case_ids = [str(row["case_id"]) for row in manifest["cases"]]
    if len(case_ids) != 39 or len(set(case_ids)) != 39:
        raise RuntimeError("Evidence case denominator or uniqueness failure")
    raw_paths: set[Path] = set()
    for case_id in case_ids:
        case = source / case_id
        for name in (
            "EVIDENCE_REPLAY_MANIFEST.json", "frozen_patchweaver_plan.json",
            "csa/evidence_bundle.json", "csa/synthesis_input.json",
            "csa/patchweaver_runtime/csa_runtime_artifacts.json",
        ):
            path = case / name
            if path.is_file():
                package.copy(path, prefix / case_id / name)
        bundle = load(case / "csa/evidence_bundle.json")
        for record in bundle["records"]:
            if record["provenance"]["origin"] != "analyzer_internal":
                continue
            raw = str(record["semantic_payload"]["raw_output_path"])
            marker = "/fse_revision/"
            if marker not in raw:
                raise ValueError(f"Internal record lacks artifact-root path: {case_id}")
            rel = Path(raw.split(marker, 1)[1])
            path = (DATA / rel).resolve()
            if not path.is_relative_to(DATA.resolve()):
                raise ValueError(f"Internal raw path escapes artifact root: {case_id}")
            raw_paths.add(path)
    for path in sorted(raw_paths):
        package.copy(path, Path("evidence") / path.relative_to(DATA))
    return case_ids


def required_repeats() -> dict[str, int]:
    names: dict[str, int] = {}
    for arm in ("native", "no_internal"):
        for stratum, cases in (("precision", 4), ("recovery", 8)):
            for rep in ("rep01", "rep02", "rep03"):
                names[f"e3_repeat12_gpt6luna_high_{arm}_{stratum}_v13_{rep}"] = cases
    for stratum, cases in (("precision", 4), ("recovery", 8)):
        for rep in ("rep02", "rep03"):
            names[f"e4_repeat12_gpt6luna_high_{stratum}_{rep}"] = cases
    for model in ("glm53", "glmflash"):
        for rep in ("rep02", "rep03"):
            suffix = "_clean" if model == "glmflash" and rep == "rep03" else ""
            names[f"e4_repeat12_{model}_anthropic_v22_precision_{rep}{suffix}"] = 4
            names[f"e4_repeat12_{model}_anthropic_v22_recovery_{rep}_clean"] = 8
    return names


def copy_motivating_example(package: Package) -> None:
    review = PROJECT / "review-revision-20260927"
    prefix = Path("evidence/motivating_example")
    selection = load(review / "FINAL_SELECTION.json")
    if selection["status"] != "selected_after_all_declared_checks":
        raise RuntimeError("Motivating example is not frozen")
    for name in ("FINAL_SELECTION.json", "EXAMPLE_SELECTION.md", "MOTIVATING_EXAMPLE_DRAFT.tex", "render_motivating_example.py"):
        package.copy(review / name, prefix / name)
    for path in sorted((review / "motivating-example/final-legible").iterdir()):
        if path.is_file():
            package.copy(path, prefix / "motivating-example/final" / path.name)
    package.copy(Path(selection["baseline_checker"]), prefix / "baseline/SAGenTestChecker.cpp")
    package.copy(DATA / "generate_only_screen_v1" / selection["case_id"] / "RESULT.json", prefix / "baseline/RESULT.json")
    for index, row in enumerate(selection["model_lineage"]):
        package.tree(Path(row["run"]), prefix / "runs" / ("initial_flash" if index == 0 else f"quality_r{index}"))
    package.tree(DATA / "motivating_g21_selection_20260927", prefix / "diagnostics")
    for name in ("prepare_motivating_g21_suite.py", "prepare_motivating_g21_confirmation.py", "prepare_motivating_g21_transfer.py", "validate_motivating_fixture_runtime.py"):
        package.copy(PROJECT / name, prefix / "scripts" / name)


def copy_backend_cost(package: Package) -> None:
    for name in ("backend_cost_20260927_v1", "backend_cost_summary_20260927_v1", "backend_integration_audit_20260927_v1"):
        if not (DATA / name / "RESULT.json").is_file():
            raise RuntimeError(f"Incomplete backend-cost audit: {name}")
        package.tree(DATA / name, Path("evidence/backend_cost") / name)
    package.copy(PROJECT / "review-revision-20260927/BACKEND_INTEGRATION_COST.md", "evidence/backend_cost/README.md")
    for name in ("measure_backend_cost.py", "audit_backend_integration.py", "summarize_backend_cost.py", "test_backend_cost.py", "run_e2_codeql_development.py", "run_e2_codeql_paired_loop.py", "validate_e2_codeql_pair.py", "test_e2_codeql_paired_loop.py"):
        package.copy(PROJECT / name, Path("evidence/backend_cost/scripts") / name)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    scan_gate = DATA / "SCAN_INTEGRITY_RELEASE_GATE.json"
    if scan_gate.is_file() and load(scan_gate).get("status") != "resolved_and_reaggregated":
        raise RuntimeError("Unresolved scan-integrity audit: repair, replay and reaggregate affected records before release")
    source_head = committed_head(SOURCE)
    auditor_head = committed_head(AUDITOR)
    paper_head = committed_head(PAPER)
    if source_head != SOURCE_EXPECTED or auditor_head != AUDITOR_EXPECTED:
        raise RuntimeError("Frozen implementation or auditor revision drifted")
    pdf = PAPER / "main.pdf"
    if not pdf.is_file():
        raise RuntimeError("The final compiled manuscript PDF is missing")
    tex_inputs = [
        path for path in PAPER.rglob("*")
        if path.is_file() and ".git" not in path.parts
        and path.suffix.lower() in {".tex", ".bib", ".sty"}
    ]
    if tex_inputs and pdf.stat().st_mtime < max(path.stat().st_mtime for path in tex_inputs):
        raise RuntimeError("The manuscript PDF predates its LaTeX sources")
    repeats = required_repeats()
    for name, expected in repeats.items():
        result = load(DATA / name / "BATCH_RESULT.json")
        if result["requested_cases"] != expected or result["completed_records"] != expected:
            raise RuntimeError(f"Incomplete repeat batch: {name}")

    output = args.output_dir.resolve()
    if output.exists():
        raise SystemExit(f"Refusing to overwrite: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=f".{output.name}.stage-", dir=output.parent))
    package = Package(stage)
    package.git_archive(SOURCE, "source/SemWeaver")
    package.git_archive(AUDITOR, "source/portfolio-auditor")
    for name, revision in HISTORICAL_IMPLS.items():
        package.git_archive(SOURCE, Path("source/frozen-implementations") / name, revision)
    package.git_archive(PAPER, "source/manuscript")
    package.copy(pdf, "paper/main.pdf")
    package.copy(PROJECT / "FSE_ARTIFACT_README_V10.md", "README.md")
    package.copy(PROJECT / "build_fse2027_review_package.py", "scripts/build_fse2027_review_package.py")
    package.copy(PROJECT / "verify_fse2027_review_package.py", "scripts/verify_fse2027_review_package.py")

    case_ids = copy_evidence(package)
    package.tree(DATA / "generate_only_materialized_v1" / "cases", "inputs/cases")
    for name in ("generate_only_v1", "generate_only_precision_cohort_v1", "generate_only_recovery_cohort_v1", "e3_e4_repeat_subset_v10"):
        package.tree(DATA / name, Path("inputs") / name)
    package.tree(DATA / "generate_only_screen_v1", "evidence/starting_screen")
    for name in SUMMARY_DIRS:
        package.tree(DATA / name, Path("results") / name)

    # Each published portfolio binds the exact complete prefix/suffix batch
    # results it used. Resolve those hashes rather than guessing a run name.
    expected_hashes: set[str] = set()
    for name in SUMMARY_DIRS:
        path = DATA / name / "RESULT.json"
        if path.is_file():
            expected_hashes.update(all_batch_hashes(load(path)))
    index: dict[str, Path] = {}
    for path in DATA.glob("*/BATCH_RESULT.json"):
        if "_quarantine_20260926" in path.parts:
            continue
        digest = sha_file(path)
        index.setdefault(digest, path.parent)
    copied_batches: set[str] = set()
    for digest in sorted(expected_hashes):
        if digest not in index:
            raise RuntimeError(f"Published portfolio source batch missing: {digest}")
        source = index[digest]
        if source.name not in copied_batches:
            package.tree(source, Path("evidence/batches") / source.name)
            copied_batches.add(source.name)
    for name in ("generate_only_knighter_gpt6luna_high_v8", "knighter_no_report25_v10"):
        if name not in copied_batches:
            package.tree(DATA / name, Path("evidence/batches") / name)
            copied_batches.add(name)
    for name in repeats:
        package.tree(DATA / name, Path("evidence/repeats") / name)
    copy_motivating_example(package)
    copy_backend_cost(package)

    e2 = DATA / "e2_imagemagick_development_v8"
    package.copy(e2 / "INPUT_MANIFEST.json", "evidence/e2_codeql/INPUT_MANIFEST.json")
    curated = WORK / "experiment_source/e2/datasets/curated/vul4c_cwe416_imagemagick_cve201712877"
    generated_query = WORK / "experiment_source/e2/runs/vul4c_cwe416_imagemagick_cve201712877/primary/codeql/20260423_151031/codeql/ImageListDeletedAliasUseAfterFree.ql"
    package.copy(generated_query, "evidence/e2_codeql/initial_generated_query.ql")
    package.copy(curated / "patches/fix.patch", "evidence/e2_codeql/fix.patch")
    for revision in ("vulnerable", "fixed"):
        package.copy(e2 / "codeql_db" / f"416_{revision}" / "codeql-database.yml", f"evidence/e2_codeql/416_{revision}_database.yml")
    for name in ("auto_loop_416_repeats_v10", "auto_loop_416_v10", "auto_loop_416_v10_r2", "auto_loop_416_v10_r3"):
        package.tree(e2 / name, Path("evidence/e2_codeql") / name)

    package.finish({
        "method": "fse2027_39_generated_only_review_package",
        "case_count": len(case_ids),
        "source_revision": source_head,
        "auditor_revision": auditor_head,
        "historical_implementation_revisions": HISTORICAL_IMPLS,
        "manuscript_revision": paper_head,
        "model_repeat_batch_count": len(repeats),
        "bound_portfolio_batch_count": len(expected_hashes),
    })
    verification = subprocess.run(
        [sys.executable, str(PROJECT / "verify_fse2027_review_package.py"), str(stage)],
        capture_output=True, text=True,
    )
    if verification.returncode != 0:
        raise RuntimeError(f"Offline package verification failed in {stage}: {verification.stderr[-1000:]}")
    stage.rename(output)
    print(json.dumps({"status": "built_and_verified", "output": str(output), "cases": len(case_ids), "repeat_batches": len(repeats), "bound_batches": len(expected_hashes)}, indent=2))


if __name__ == "__main__":
    main()
