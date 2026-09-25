#!/usr/bin/env python3
"""Build a compact, path-redacted FSE replication package."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import re
import shutil
import subprocess
import tarfile
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
LLM_NATIVE = REPO.parent
ARTIFACTS = LLM_NATIVE / "artifacts" / "fse_revision"
INPUTS = LLM_NATIVE / "experiment_source" / "knighter" / "e1"
MANUSCRIPT = LLM_NATIVE.parent / "manuscript"
LINUX = REPO / "artifacts" / "external" / "linux"

EVIDENCE_TREES = (
    "case07_negative_control",
    "case05_matched_knighter_metamorphic",
    "case08_signed_literal_sensitivity",
    "case08_assignment_sanity",
    "case19_three_arg_sensitivity",
    "arity_sensitivity_matched_v3",
    "arity_sensitivity_full_v3",
    "arity_sensitivity_targeted_v2",
    "case23_bounded_consumer_sensitivity",
    "matched_knighter_gpt56terra_cohort_v2",
    "semweaver_treatment_evidence_cohort_v3",
    "semweaver_treatment_matched_v3",
    "semweaver_treatment_full_v3",
    "semweaver_targeted_postgate_v2",
)

TEXT_SUFFIXES = {
    ".c", ".cpp", ".csv", ".h", ".json", ".jsonl", ".log", ".md",
    ".patch", ".txt", ".yaml", ".yml",
}
EXCLUDED_PARTS = {".git", "CMakeFiles", "build", "scan-reports-0", "__pycache__"}
CASE_INPUTS = (
    "csa/SAGenTestChecker.cpp",
    "evidence_manifest.json",
    "fixed_validation.json",
    "metadata/candidate.json",
    "metadata/pattern.txt",
    "metadata/plan.txt",
    "metadata/refined_plan.txt",
    "patchweaver_plan.json",
    "patches/commit.patch",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--archive", type=Path)
    return parser.parse_args()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git_value(repo: Path, *args: str) -> str:
    return subprocess.run(
        ["git", "-C", str(repo), *args],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()


def export_git(repo: Path, destination: Path) -> None:
    archive = subprocess.run(
        ["git", "-C", str(repo), "archive", "HEAD"],
        check=True,
        capture_output=True,
    ).stdout
    destination.mkdir(parents=True, exist_ok=True)
    with tarfile.open(fileobj=io.BytesIO(archive), mode="r:") as handle:
        handle.extractall(destination)


def redactions() -> tuple[tuple[str, str, str], ...]:
    return (
        ("local_workspace", str(LLM_NATIVE), "/artifact"),
        ("local_project_root", str(LLM_NATIVE.parent), "/artifact-root"),
        ("local_home", str(Path.home()), "/anonymous/home"),
        ("local_account", Path.home().name, "anonymous"),
    )


def copy_redacted(source: Path, destination: Path, redaction_log: list[dict]) -> None:
    raw = source.read_bytes()
    text = raw.decode("utf-8", errors="strict")
    rewritten = text
    applied: list[str] = []
    for label, old, new in redactions():
        if old in rewritten:
            rewritten = rewritten.replace(old, new)
            applied.append(label)
    scratch_rewritten = re.sub(r"/external/upstream-study/\s\"']+", "/external/upstream-study", rewritten)
    if scratch_rewritten != rewritten:
        rewritten = scratch_rewritten
        applied.append("scratch_account")
    home_rewritten = re.sub(r"/(?:home|Users)/[^/\s\"']+", "/anonymous/home", rewritten)
    if home_rewritten != rewritten:
        rewritten = home_rewritten
        applied.append("generic_home_account")
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(rewritten, encoding="utf-8")
    if applied:
        redaction_log.append(
            {
                "path": destination.as_posix(),
                "original_sha256": hashlib.sha256(raw).hexdigest(),
                "redacted_sha256": sha256(destination),
                "replacements": applied,
            }
        )


def copy_tree(source: Path, destination: Path, redaction_log: list[dict]) -> None:
    for path in sorted(source.rglob("*")):
        if not path.is_file():
            continue
        relative = path.relative_to(source)
        if any(part in EXCLUDED_PARTS for part in relative.parts):
            continue
        if path.suffix.lower() not in TEXT_SUFFIXES:
            continue
        copy_redacted(path, destination / relative, redaction_log)


def copy_inputs(destination: Path, redaction_log: list[dict]) -> list[str]:
    batch = json.loads(
        (ARTIFACTS / "semweaver_treatment_evidence_cohort_v3" / "BATCH_MANIFEST.json").read_text(
            encoding="utf-8"
        )
    )
    case_ids = [str(item["case_id"]) for item in batch["cases"]]
    for case_id in case_ids:
        source_case = INPUTS / "cases" / case_id
        for relative in CASE_INPUTS:
            source = source_case / relative
            if source.exists():
                copy_redacted(source, destination / "cases" / case_id / relative, redaction_log)
    for name in ("FINAL_SAMPLE_PROTOCOL.md", "RUNBOOK.md", "summary.json"):
        source = INPUTS / name
        if source.exists():
            copy_redacted(source, destination / name, redaction_log)
    manifest_dir = INPUTS / "manifests"
    if manifest_dir.exists():
        copy_tree(manifest_dir, destination / "manifests", redaction_log)
    return case_ids


def copy_vulnerable_sources(destination: Path, case_ids: list[str]) -> None:
    """Retain minimal public Linux blobs for offline revision binding checks."""
    evidence = ARTIFACTS / "semweaver_treatment_evidence_cohort_v3"
    binding_rows = {
        row["case_id"]: row
        for row in json.loads((evidence / "BINDING_AUDIT.json").read_text(encoding="utf-8"))["rows"]
    }
    blobs = []
    for case_id in case_ids:
        replay = json.loads(
            (evidence / case_id / "EVIDENCE_REPLAY_MANIFEST.json").read_text(encoding="utf-8")
        )
        prep = replay["preparation"]
        revision = str(prep["revision"])
        if revision != f"{prep['commit_id']}^":
            raise ValueError(f"Unexpected vulnerable revision for {case_id}")
        for relative in binding_rows[case_id]["patch_files"]:
            spec = f"{revision}:{relative}"
            source = subprocess.run(
                ["git", "-C", str(LINUX), "show", spec],
                check=True,
                capture_output=True,
            ).stdout
            oid = subprocess.run(
                ["git", "-C", str(LINUX), "rev-parse", spec],
                check=True,
                capture_output=True,
                text=True,
            ).stdout.strip()
            target = destination / "vulnerable_source" / case_id / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(source)
            blobs.append(
                {
                    "case_id": case_id,
                    "commit_id": prep["commit_id"],
                    "revision": revision,
                    "path": relative,
                    "git_blob_sha1": oid,
                    "sha256": hashlib.sha256(source).hexdigest(),
                    "size": len(source),
                }
            )
    (destination / "VULNERABLE_SOURCE_BLOBS.json").write_text(
        json.dumps({"schema_version": 1, "blob_count": len(blobs), "blobs": blobs}, indent=2)
        + "\n",
        encoding="utf-8",
    )


def write_readme(destination: Path, case_ids: list[str]) -> None:
    destination.write_text(
        """# SemWeaver FSE replication package

This package supports the completed 12-subject CSA measurements reported in the
paper. It intentionally labels the full-budget and targeted runs as development
conditions; it does not claim confirmatory SemWeaver effectiveness.

Contents:

- `source/SemWeaver/`: implementation, prompts, tests, and experiment drivers.
- `source/manuscript/`: committed paper sources; `paper/main.pdf` is the built PDF.
- `inputs/e1/`: the 12 frozen checker/patch/plan inputs, cohort manifests, and
  minimal vulnerable-revision Linux source blobs with Git object identifiers.
- `evidence/`: compact raw and derived artifacts. Build products, scan-view HTML,
  object files, and plugins are excluded; source, prompts/responses available from
  the upstream baseline, decision traces, candidates, manifests, validator logs,
  CSA raw text, and metamorphic outcomes are retained.
- `evidence/case08_*`, `evidence/case19_three_arg_sensitivity`,
  `evidence/arity_sensitivity_*`, and
  `evidence/case23_bounded_consumer_sensitivity`: post-hoc development
  sensitivity audits of fixture fidelity. They are not fresh-subject or
  independent-author confirmation and never enter automatic PDS numerators.
- `ARTIFACT_MANIFEST.json`: SHA-256 and size for every packaged file.
- `REDACTIONS.json`: path-only redactions, with original and packaged digests.
  The verifier uses these mappings to check the original frozen hashes even
  when local paths in the packaged copy were anonymized.

Run `python3 source/SemWeaver/scripts/verify_fse_artifact.py .` from this directory
for a no-network integrity and result-consistency check. Re-executing Linux/CSA
runs requires the documented Clang 18 environment and public Linux revisions.

The completed SemWeaver runs predate full exchange logging: they retain parsed
actions and every applied candidate, but not every rendered treatment prompt and
raw provider response. The implementation now records those exchanges for future
runs; this limitation is stated in the paper.

Historical files under `inputs/e1/` may describe manual repair and backfill in
the earlier study. Those legacy outcomes are excluded from every reported
automatic numerator. Current outcomes are rebuilt from the separately frozen
`matched_knighter_gpt56terra_cohort_v2`, `semweaver_treatment_matched_v3`, and
`semweaver_treatment_full_v3` batches. The verifier checks their case identities,
candidate hashes, validator results, and adoption counts.
""",
        encoding="utf-8",
    )


def build_manifest(root: Path, metadata: dict) -> dict:
    files = []
    for path in sorted(root.rglob("*")):
        if not path.is_file() or path.name == "ARTIFACT_MANIFEST.json":
            continue
        files.append(
            {
                "path": path.relative_to(root).as_posix(),
                "size": path.stat().st_size,
                "sha256": sha256(path),
            }
        )
    return {"schema_version": 1, **metadata, "file_count": len(files), "files": files}


def main() -> int:
    args = parse_args()
    output = args.output_dir.expanduser().resolve()
    if output.exists():
        raise SystemExit(f"Refusing to overwrite existing output directory: {output}")
    output.mkdir(parents=True)

    redaction_log: list[dict] = []
    export_git(REPO, output / "source" / "SemWeaver")
    export_git(MANUSCRIPT, output / "source" / "manuscript")
    (output / "paper").mkdir(parents=True, exist_ok=True)
    shutil.copy2(MANUSCRIPT / "main.pdf", output / "paper" / "main.pdf")

    case_ids = copy_inputs(output / "inputs" / "e1", redaction_log)
    copy_vulnerable_sources(output / "inputs" / "e1", case_ids)
    for name in EVIDENCE_TREES:
        copy_tree(ARTIFACTS / name, output / "evidence" / name, redaction_log)

    write_readme(output / "README.md", case_ids)
    for item in redaction_log:
        item["path"] = Path(item["path"]).relative_to(output).as_posix()
    (output / "REDACTIONS.json").write_text(
        json.dumps(
            {
                "schema_version": 1,
                "policy": "Path and local-account identifiers only; program semantics and measurements are unchanged.",
                "file_count": len(redaction_log),
                "files": redaction_log,
            },
            indent=2,
            ensure_ascii=False,
        )
        + "\n",
        encoding="utf-8",
    )

    metadata = {
        "semweaver_revision": git_value(REPO, "rev-parse", "HEAD"),
        "manuscript_revision": git_value(MANUSCRIPT, "rev-parse", "HEAD"),
        "case_count": len(case_ids),
        "included_evidence_trees": list(EVIDENCE_TREES),
    }
    manifest = build_manifest(output, metadata)
    (output / "ARTIFACT_MANIFEST.json").write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )

    if args.archive:
        archive = args.archive.expanduser().resolve()
        archive.parent.mkdir(parents=True, exist_ok=True)
        with tarfile.open(archive, mode="w:gz") as handle:
            handle.add(output, arcname=output.name)
        print(f"archive={archive} sha256={sha256(archive)}")
    print(f"output={output} files={manifest['file_count']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
