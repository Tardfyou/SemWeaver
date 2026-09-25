#!/usr/bin/env python3
"""Replay one frozen Knighter patch plan through the current CSA collectors."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import subprocess
import sys
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[3]
BASELINE_SRC = PROJECT_ROOT / "experiments" / "knighter" / "baseline" / "src"
sys.path.insert(0, str(PROJECT_ROOT))
sys.path.insert(0, str(BASELINE_SRC))

from src.core.analyzer_base import AnalyzerContext
from src.core.csa_analyzer import CSAAnalyzer
from src.utils import load_config
from targets.linux import Linux


STALE_EVIDENCE_KEYS = (
    "evidence_bundle",
    "refinement_evidence_bundles",
    "validation_feedback",
    "validation_feedback_history",
    "post_validation_evidence_bundle",
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def clean_frozen_plan(raw_plan: dict) -> dict:
    plan = copy.deepcopy(raw_plan or {})
    patchweaver = dict(plan.get("patchweaver", {}) or {})
    for key in STALE_EVIDENCE_KEYS:
        patchweaver.pop(key, None)
        plan.pop(key, None)
    plan["patchweaver"] = patchweaver
    return plan


def origin_counts(bundle: dict) -> dict:
    counts: dict[str, int] = {}
    for record in list(bundle.get("records", []) or []):
        provenance = record.get("provenance", {}) or {}
        origin = str(provenance.get("origin", "unknown") or "unknown")
        counts[origin] = counts.get(origin, 0) + 1
    return dict(sorted(counts.items()))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--case-dir", required=True, type=Path)
    parser.add_argument("--linux-dir", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument(
        "--prepare-vulnerable",
        action="store_true",
        help="Checkout the vulnerable revision, build patch objects, and regenerate compile_commands.json",
    )
    return parser.parse_args()


def prepare_vulnerable_revision(
    *, case_dir: Path, linux_dir: Path, output_dir: Path, jobs: int
) -> dict:
    metadata_path = case_dir / "metadata" / "candidate.json"
    patch_path = case_dir / "patches" / "knighter_patch.md"
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    commit_id = str(metadata.get("commit_id", "") or "")
    if not commit_id:
        raise ValueError("candidate metadata has no commit_id")
    objects = Linux.get_objects_from_patch(patch_path.read_text(encoding="utf-8"))
    if not objects:
        raise ValueError("frozen patch resolves no C build objects")

    prep_dir = output_dir / "preparation"
    prep_dir.mkdir(parents=True, exist_ok=True)
    compile_db = linux_dir / "compile_commands.json"
    compile_db.unlink(missing_ok=True)
    target = Linux(str(linux_dir))
    target.checkout_commit(commit_id, is_before=True)
    build = subprocess.run(
        ["make", "LLVM=1", "ARCH=x86", f"-j{jobs}", *objects],
        cwd=str(linux_dir),
        capture_output=True,
        text=True,
        timeout=1800,
        check=False,
    )
    (prep_dir / "build.stdout.log").write_text(build.stdout, encoding="utf-8")
    (prep_dir / "build.stderr.log").write_text(build.stderr, encoding="utf-8")
    if build.returncode != 0:
        raise RuntimeError(f"vulnerable object build failed with return code {build.returncode}")

    generator = linux_dir / "scripts" / "clang-tools" / "gen_compile_commands.py"
    generated = subprocess.run(
        [sys.executable, str(generator), "-d", str(linux_dir)],
        cwd=str(linux_dir),
        capture_output=True,
        text=True,
        timeout=300,
        check=False,
    )
    (prep_dir / "compile_commands.stdout.log").write_text(
        generated.stdout, encoding="utf-8"
    )
    (prep_dir / "compile_commands.stderr.log").write_text(
        generated.stderr, encoding="utf-8"
    )
    if generated.returncode != 0 or not compile_db.is_file():
        raise RuntimeError("compile_commands.json generation failed")
    return {
        "commit_id": commit_id,
        "revision": f"{commit_id}^",
        "objects": objects,
        "compile_commands_sha256": sha256(compile_db),
        "build_return_code": build.returncode,
        "generator_return_code": generated.returncode,
    }


def main() -> int:
    args = parse_args()
    case_dir = args.case_dir.resolve()
    linux_dir = args.linux_dir.resolve()
    output_dir = args.output_dir.resolve()
    patch_path = case_dir / "patches" / "commit.patch"
    checker_path = case_dir / "csa" / "SAGenTestChecker.cpp"
    source_plan_path = case_dir / "patchweaver_plan.json"
    for path in (patch_path, checker_path, source_plan_path):
        if not path.is_file():
            raise FileNotFoundError(path)
    if not linux_dir.is_dir():
        raise NotADirectoryError(linux_dir)

    csa_output = output_dir / "csa"
    csa_output.mkdir(parents=True, exist_ok=True)
    raw_plan = json.loads(source_plan_path.read_text(encoding="utf-8"))
    shared_analysis = clean_frozen_plan(raw_plan)
    frozen_plan_path = output_dir / "frozen_patchweaver_plan.json"
    frozen_plan_path.write_text(
        json.dumps(shared_analysis, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )

    preparation = None
    if args.prepare_vulnerable:
        preparation = prepare_vulnerable_revision(
            case_dir=case_dir,
            linux_dir=linux_dir,
            output_dir=output_dir,
            jobs=args.jobs,
        )

    config = load_config(str(args.config.resolve()))
    analyzer = CSAAnalyzer(config=config, suppress_output=True)
    context = AnalyzerContext(
        patch_path=str(patch_path),
        output_dir=str(csa_output),
        evidence_dir=str(linux_dir),
        shared_analysis=shared_analysis,
    )
    bundle = analyzer.collect_evidence(context)
    synthesis = analyzer.build_synthesis_input(context, bundle)
    bundle_path = csa_output / "evidence_bundle.json"
    synthesis_path = csa_output / "synthesis_input.json"
    bundle_payload = bundle.to_dict()
    bundle_path.write_text(
        json.dumps(bundle_payload, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    synthesis_path.write_text(
        json.dumps(synthesis.to_dict(), indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )

    counts = origin_counts(bundle_payload)
    native_records = int(counts.get("analyzer_internal", 0))
    eligible = native_records > 0
    manifest = {
        "schema_version": 1,
        "method": "frozen_plan_csa_evidence_replay",
        "case_id": case_dir.name,
        "source_plan_sha256": sha256(source_plan_path),
        "frozen_plan_sha256": sha256(frozen_plan_path),
        "patch_sha256": sha256(patch_path),
        "starting_checker_sha256": sha256(checker_path),
        "evidence_bundle_sha256": sha256(bundle_path),
        "synthesis_input_sha256": sha256(synthesis_path),
        "records": len(bundle_payload.get("records", []) or []),
        "origin_counts": counts,
        "analyzer_internal_records": native_records,
        "eligible": eligible,
        "decision": "treatment_eligible" if eligible else "abstain_missing_analyzer_internal",
        "stale_evidence_keys_removed": list(STALE_EVIDENCE_KEYS),
        "new_llm_calls": 0,
        "preparation": preparation,
    }
    (output_dir / "EVIDENCE_REPLAY_MANIFEST.json").write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(manifest, indent=2, ensure_ascii=False))
    return 0 if eligible else 2


if __name__ == "__main__":
    raise SystemExit(main())
