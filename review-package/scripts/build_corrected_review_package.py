#!/usr/bin/env python3
"""Build the final scan-corrected review package; raw superseded runs are labelled."""
import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path
import build_fse2027_review_package as base


def roots_from(value):
    roots = set()
    if isinstance(value, dict):
        for key, item in value.items():
            if key in {"validation_path", "old_result", "result"} and isinstance(item, str) and "/" in item:
                roots.add(item.split("/", 1)[0])
            elif key == "source_batch" and isinstance(item, str):
                roots.add(item)
            roots.update(roots_from(item))
    elif isinstance(value, list):
        for item in value:
            roots.update(roots_from(item))
    return roots


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--summary", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    summary_path = args.summary.resolve()
    corrected = base.load(summary_path)
    if corrected["starting"]["metrics"]["cases"] != 39:
        raise ValueError("Wrong corrected denominator")
    source = base.WORK / "SemWeaver-v26"
    source_head = base.committed_head(source)
    paper_head = base.committed_head(base.PAPER)
    pdf = base.PAPER / "main.pdf"
    inputs = [p for p in base.PAPER.rglob("*") if p.is_file() and ".git" not in p.parts and p.suffix in {".tex", ".bib", ".sty"}]
    if not pdf.exists() or pdf.stat().st_mtime < max(p.stat().st_mtime for p in inputs):
        raise ValueError("Paper PDF missing or older than source")
    target = args.output_dir.resolve()
    if target.exists():
        raise FileExistsError(target)
    stage = Path(tempfile.mkdtemp(prefix=f".{target.name}.stage-", dir=target.parent))
    package = base.Package(stage)
    package.git_archive(source, "source/SemWeaver")
    historical = {**base.HISTORICAL_IMPLS, "scanfix_v24": "07e76c0", "plugin_load_v25": "8a6351e"}
    for name, revision in historical.items():
        package.git_archive(source, Path("source/frozen-implementations") / name, revision)
    package.git_archive(base.PAPER, "source/manuscript")
    package.copy(pdf, "paper/main.pdf")
    package.copy(base.PROJECT / "verify_corrected_review_package.py", "scripts/verify_corrected_review_package.py")
    package.copy(base.PROJECT / "verify_fse2027_review_package.py", "scripts/verify_fse2027_review_package.py")
    package.copy(base.PROJECT / "reaggregate_scanfix.py", "scripts/reaggregate_scanfix.py")
    package.copy(base.PROJECT / "build_corrected_review_package.py", "scripts/build_corrected_review_package.py")
    package.copy(base.PROJECT / "build_fse2027_review_package.py", "scripts/build_fse2027_review_package.py")
    package.copy(base.PROJECT / "clang18.Dockerfile", "environment/clang18.Dockerfile")
    package.tree(base.DATA / "final_runtime_v26", "environment/measured")
    package.copy(summary_path, "results/corrected/RESULT.json")
    package.copy(summary_path.with_name("SUMMARY.csv"), "results/corrected/SUMMARY.csv")
    base.copy_evidence(package)
    package.tree(base.DATA / "generate_only_materialized_v1/cases", "inputs/cases")
    package.tree(base.DATA / "generate_only_v1", "inputs/generate_only_v1")
    package.tree(base.DATA / "e3_e4_repeat_subset_v10", "inputs/original_repeat_subset")
    roots = roots_from(corrected)
    roots.update({"generate_only_screen_v1", "generate_only_knighter_gpt6luna_high_v8", "generate_only_knighter_gpt6luna_high_v8_strict_v1", "scanfix_v24_knighter_g08", "scanfix_v24_knighter_g08_strict", "scanfix_v24_knighter_g08_inputs", "scan_integrity_audit_v24"})
    roots.update({"e4_all39_gpt6luna_high_portfolio_v19", "e4_all39_glm53_mixed_wire_portfolio_v20", "e4_all39_glmflash_mixed_wire_portfolio_v20", "e3_all39_gpt6luna_high_native_v13_portfolio", "e3_all39_gpt6luna_high_no_internal_v13_portfolio", "all39_knighter_system_v10", "e1_matched14_gpt6luna_high_native_comparison_v20", "e1_matched14_gpt6luna_high_no_internal_comparison_v20", "e3_e4_repeat_subset_v10"})
    for pattern in ("scanfix_v24_g08_*queue*", "scanfix_v25_remaining_*queue", "scanfix_v26_*queue*", "scanfix_v24_replay_shard*"):
        roots.update(p.name for p in base.DATA.glob(pattern) if p.is_dir())
    for name in sorted(roots):
        package.tree(base.DATA / name, Path("evidence/fse_revision") / name)
    for row in corrected["starting"]["rows"]:
        package.copy(base.DATA / row["validation_path"], Path("inputs/corrected-starting") / row["case_id"] / "RESULT.json")
    base.copy_motivating_example(package)
    base.copy_backend_cost(package)
    e2 = base.DATA / "e2_imagemagick_development_v8"
    generated_query = base.WORK / "experiment_source/e2/runs/vul4c_cwe416_imagemagick_cve201712877/primary/codeql/20260423_151031/codeql/ImageListDeletedAliasUseAfterFree.ql"
    package.copy(generated_query, "evidence/e2_codeql/initial_generated_query.ql")
    package.copy(base.WORK / "experiment_source/e2/datasets/curated/vul4c_cwe416_imagemagick_cve201712877/patches/fix.patch", "evidence/e2_codeql/fix.patch")
    for side in ("vulnerable", "fixed"):
        package.copy(e2 / "codeql_db" / f"416_{side}/codeql-database.yml", f"evidence/e2_codeql/416_{side}_database.yml")
    for name in ("auto_loop_416_repeats_v10", "auto_loop_416_v10", "auto_loop_416_v10_r2", "auto_loop_416_v10_r3"):
        package.tree(e2 / name, Path("evidence/e2_codeql") / name)
    package.copy(base.PROJECT / "CORRECTED_ARTIFACT_README.md", "README.md")
    linux = base.WORK / "SemWeaver/artifacts/external/linux"
    package.copy(linux / "COPYING", "licenses/Linux-COPYING.txt")
    package.copy(linux / "LICENSES/preferred/GPL-2.0", "licenses/Linux-GPL-2.0.txt")
    package.copy(base.PROJECT / "ARTIFACT_EVIDENCE_SCOPE.md", "docs/EVIDENCE_SCOPE.md")
    package.copy(base.PROJECT / "review-revision-20260927/TARGET_ALIGNMENT_NOTES.md", "docs/TARGET_ALIGNMENT_NOTES.md")
    package.finish({"method": "fse2027_scan_corrected_review_package", "case_count": 39, "precision_cases": 15, "recovery_cases": 24, "source_revision": source_head, "manuscript_revision": paper_head, "corrected_summary_sha256": base.sha_file(summary_path), "raw_run_root_count": len(roots)})
    # Verify the exact exported scripts, not only the publisher's local copies.
    result = subprocess.run([sys.executable, str(stage / "scripts/verify_corrected_review_package.py"), str(stage)], capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f"Package remains unpublished at {stage}; verifier failed: {result.stderr[-1500:]}")
    stage.rename(target)
    print(json.dumps({"status": "built_and_verified", "path": str(target), "source_revision": source_head, "paper_revision": paper_head, "verification": json.loads(result.stdout)}, indent=2))


if __name__ == "__main__":
    main()
