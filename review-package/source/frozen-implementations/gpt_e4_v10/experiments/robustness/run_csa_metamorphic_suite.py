#!/usr/bin/env python3
"""Build a frozen CSA checker and score a manifest of C metamorphic fixtures."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--checker", required=True, type=Path)
    parser.add_argument("--suite-dir", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--clang-root", type=Path, default=Path("/usr/lib/llvm-18"))
    parser.add_argument("--jobs", type=int, default=8)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    checker = args.checker.resolve()
    suite_dir = args.suite_dir.resolve()
    output_dir = args.output_dir.resolve()
    build_dir = output_dir / "build"
    raw_dir = output_dir / "raw_logs"
    build_dir.mkdir(parents=True, exist_ok=True)
    raw_dir.mkdir(parents=True, exist_ok=True)

    with (suite_dir / "manifest.csv").open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))
    frozen_manifest = {
        "checker_sha256": sha256(checker),
        "manifest_sha256": sha256(suite_dir / "manifest.csv"),
        "fixtures": {
            row["variant_id"]: sha256(suite_dir / row["source_path"]) for row in rows
        },
    }
    (output_dir / "FROZEN_MANIFEST.json").write_text(
        json.dumps(frozen_manifest, indent=2) + "\n", encoding="utf-8"
    )

    configure = [
        "cmake",
        "-S",
        str(ROOT / "experiments" / "robustness" / "case07_negative_control"),
        "-B",
        str(build_dir),
        "-G",
        "Ninja",
        f"-DCMAKE_CXX_COMPILER={args.clang_root / 'bin' / 'clang++'}",
        f"-DCHECKER_SOURCE={checker}",
        f"-DUTILITY_SOURCE={ROOT / 'experiments' / 'knighter' / 'baseline' / 'llvm_utils' / 'utility.cpp'}",
        f"-DUTILITY_HEADER={ROOT / 'experiments' / 'knighter' / 'baseline' / 'llvm_utils' / 'utility.h'}",
        f"-DLLVM_DIR={args.clang_root / 'lib' / 'cmake' / 'llvm'}",
        f"-DClang_DIR={args.clang_root / 'lib' / 'cmake' / 'clang'}",
    ]
    configured = subprocess.run(configure, capture_output=True, text=True, check=False)
    (output_dir / "configure.stdout.log").write_text(configured.stdout, encoding="utf-8")
    (output_dir / "configure.stderr.log").write_text(configured.stderr, encoding="utf-8")
    if configured.returncode != 0:
        raise RuntimeError("Checker suite CMake configuration failed")
    built = subprocess.run(
        ["cmake", "--build", str(build_dir), "-j", str(args.jobs)],
        capture_output=True,
        text=True,
        check=False,
    )
    (output_dir / "build.stdout.log").write_text(built.stdout, encoding="utf-8")
    (output_dir / "build.stderr.log").write_text(built.stderr, encoding="utf-8")
    if built.returncode != 0:
        raise RuntimeError("Checker suite plugin build failed")
    plugin = build_dir / "SAGenTestPlugin.so"

    results = []
    for row in rows:
        source = suite_dir / row["source_path"]
        command = [
            str(args.clang_root / "bin" / "clang"),
            "--analyze",
            "-fno-color-diagnostics",
            "-Xclang",
            "-load",
            "-Xclang",
            str(plugin),
            "-Xclang",
            "-analyzer-checker=custom.SAGenTestChecker",
            str(source),
        ]
        completed = subprocess.run(command, capture_output=True, text=True, check=False)
        combined = completed.stdout + completed.stderr
        (raw_dir / f"{row['variant_id']}.log").write_text(combined, encoding="utf-8")
        alert_count = combined.count("[custom.SAGenTestChecker]")
        expected = bool(int(row["expected_alert"]))
        observed = alert_count > 0
        results.append(
            {
                **row,
                "source_sha256": sha256(source),
                "return_code": completed.returncode,
                "alert_count": alert_count,
                "observed_alert": observed,
                "passed": completed.returncode == 0 and observed == expected,
            }
        )

    positive = [row for row in results if row["variant_kind"] == "positive"]
    negative = [row for row in results if row["variant_kind"] == "negative"]
    summary = {
        "schema_version": 1,
        **frozen_manifest,
        "clang": subprocess.run(
            [str(args.clang_root / "bin" / "clang"), "--version"],
            capture_output=True,
            text=True,
            check=False,
        ).stdout.splitlines()[0],
        "positive_passed": sum(row["passed"] for row in positive),
        "positive_total": len(positive),
        "negative_passed": sum(row["passed"] for row in negative),
        "negative_total": len(negative),
        "robust": all(row["passed"] for row in results),
        "results": results,
    }
    (output_dir / "RESULT.json").write_text(
        json.dumps(summary, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    print(json.dumps(summary, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
