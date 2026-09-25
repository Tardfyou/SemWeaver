#!/usr/bin/env python3
"""Run a compiled CSA checker plugin over a frozen fixture manifest."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import subprocess
from pathlib import Path
from typing import Dict, List


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_bool(value: str) -> bool:
    return str(value or "").strip().lower() in {"1", "true", "yes"}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--plugin", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--clang", default="clang-18")
    parser.add_argument("--checker", default="custom.SAGenTestChecker")
    parser.add_argument("--timeout", type=int, default=120)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    manifest = args.manifest.expanduser().resolve()
    plugin = args.plugin.expanduser().resolve()
    output_dir = args.output_dir.expanduser().resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    with manifest.open(encoding="utf-8-sig", newline="") as handle:
        variants = list(csv.DictReader(handle))

    results: List[Dict[str, object]] = []
    for variant in variants:
        source = (manifest.parent / str(variant["source_path"])).resolve()
        variant_id = str(variant["variant_id"])
        expected = parse_bool(str(variant["expected_alert"]))
        command = [
            args.clang,
            "--analyze",
            "-Xclang",
            "-load",
            "-Xclang",
            str(plugin),
            "-Xclang",
            f"-analyzer-checker={args.checker}",
            "-Xclang",
            "-analyzer-output=text",
            str(source),
        ]
        try:
            process = subprocess.run(
                command,
                capture_output=True,
                text=True,
                timeout=args.timeout,
                check=False,
            )
            output = (process.stdout or "") + (process.stderr or "")
            build_ok = process.returncode == 0
        except subprocess.TimeoutExpired as exc:
            output = (exc.stdout or "") + (exc.stderr or "")
            process = None
            build_ok = False

        log_path = output_dir / f"{variant_id}.log"
        log_path.write_text(output, encoding="utf-8")
        alert_lines = [
            line
            for line in output.splitlines()
            if args.checker in line or "Conversion failure jumps to free_fc" in line
        ]
        observed = bool(alert_lines)
        passed = build_ok and observed == expected
        results.append(
            {
                "variant_id": variant_id,
                "variant_kind": variant["variant_kind"],
                "transformation": variant["transformation"],
                "source_path": str(source),
                "source_sha256": sha256(source),
                "expected_alert": expected,
                "observed_alert": observed,
                "alert_line_count": len(alert_lines),
                "build_ok": build_ok,
                "passed": passed,
                "return_code": process.returncode if process is not None else None,
                "log_path": str(log_path),
                "intervention": "automatic",
            }
        )

    positive = [row for row in results if row["variant_kind"] == "positive"]
    negative = [row for row in results if row["variant_kind"] == "negative"]
    summary = {
        "schema_version": 1,
        "manifest": str(manifest),
        "manifest_sha256": sha256(manifest),
        "plugin": str(plugin),
        "plugin_sha256": sha256(plugin),
        "checker": args.checker,
        "variant_count": len(results),
        "positive_passed": sum(bool(row["passed"]) for row in positive),
        "positive_total": len(positive),
        "negative_passed": sum(bool(row["passed"]) for row in negative),
        "negative_total": len(negative),
        "all_builds_ok": all(bool(row["build_ok"]) for row in results),
        "robust": all(bool(row["passed"]) for row in results),
        "intervention": "automatic",
    }
    report = {"summary": summary, "variants": results}
    (output_dir / "metamorphic_report.json").write_text(
        json.dumps(report, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(summary, indent=2, ensure_ascii=False))
    return 0 if summary["robust"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
