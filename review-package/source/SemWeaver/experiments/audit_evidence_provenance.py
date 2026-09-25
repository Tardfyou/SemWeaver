#!/usr/bin/env python3
"""Audit the actual origin of persisted SemWeaver evidence records.

The audit deliberately distinguishes analyzer-internal facts from analyzer
diagnostics, patch/source-derived fallbacks, behavioral feedback, and manual or
backfilled records.  It is intended to make paper denominators reproducible and
to prevent an ``analyzer=csa`` label from being reported as native evidence.
"""

from __future__ import annotations

import argparse
import csv
import json
import sys
from collections import Counter
from pathlib import Path
from typing import Any, Dict, Iterable, List


PROJECT_ROOT = Path(__file__).resolve().parents[1]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from src.evidence_provenance import (  # noqa: E402
    EVIDENCE_ORIGIN_ANALYZER_INTERNAL,
    EVIDENCE_ORIGIN_ANALYZER_OUTPUT,
    EVIDENCE_ORIGIN_BEHAVIORAL,
    EVIDENCE_ORIGIN_MANUAL,
    EVIDENCE_ORIGIN_SOURCE_DERIVED,
    EVIDENCE_ORIGIN_UNKNOWN,
    classify_evidence_origin,
)


ORIGIN_COLUMNS = (
    EVIDENCE_ORIGIN_ANALYZER_INTERNAL,
    EVIDENCE_ORIGIN_ANALYZER_OUTPUT,
    EVIDENCE_ORIGIN_SOURCE_DERIVED,
    EVIDENCE_ORIGIN_BEHAVIORAL,
    EVIDENCE_ORIGIN_MANUAL,
    EVIDENCE_ORIGIN_UNKNOWN,
)


def discover_bundles(inputs: Iterable[Path]) -> List[Path]:
    paths: set[Path] = set()
    for raw_path in inputs:
        path = raw_path.expanduser().resolve()
        if path.is_file():
            paths.add(path)
            continue
        if path.is_dir():
            paths.update(path.rglob("evidence_bundle.json"))
    return sorted(paths)


def classify_record(record: Dict[str, Any]) -> str:
    provenance = record.get("provenance", {}) or {}
    explicit = str(provenance.get("origin", "") or "").strip()
    if explicit in ORIGIN_COLUMNS:
        return explicit
    return classify_evidence_origin(
        str(provenance.get("tool", "") or ""),
        str(provenance.get("artifact", "") or ""),
    )


def audit_bundle(path: Path) -> Dict[str, Any]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    records = list(payload.get("records", []) or [])
    counts = Counter(classify_record(record) for record in records)
    total = len(records)
    internal = counts[EVIDENCE_ORIGIN_ANALYZER_INTERNAL]
    analyzer_backed = internal + counts[EVIDENCE_ORIGIN_ANALYZER_OUTPUT]
    source_derived = counts[EVIDENCE_ORIGIN_SOURCE_DERIVED]
    return {
        "bundle_path": str(path),
        "total_records": total,
        **{origin: counts[origin] for origin in ORIGIN_COLUMNS},
        "analyzer_internal_fraction": (internal / total) if total else 0.0,
        "analyzer_backed_fraction": (analyzer_backed / total) if total else 0.0,
        "source_derived_fraction": (source_derived / total) if total else 0.0,
        "native_claim_supported": bool(internal),
    }


def aggregate(rows: List[Dict[str, Any]]) -> Dict[str, Any]:
    total = sum(int(row["total_records"]) for row in rows)
    counts = {
        origin: sum(int(row[origin]) for row in rows)
        for origin in ORIGIN_COLUMNS
    }
    internal = counts[EVIDENCE_ORIGIN_ANALYZER_INTERNAL]
    analyzer_backed = internal + counts[EVIDENCE_ORIGIN_ANALYZER_OUTPUT]
    source_derived = counts[EVIDENCE_ORIGIN_SOURCE_DERIVED]
    return {
        "bundle_count": len(rows),
        "total_records": total,
        "origin_counts": counts,
        "analyzer_internal_fraction": (internal / total) if total else 0.0,
        "analyzer_backed_fraction": (analyzer_backed / total) if total else 0.0,
        "source_derived_fraction": (source_derived / total) if total else 0.0,
        "bundles_with_native_records": sum(bool(row["native_claim_supported"]) for row in rows),
    }


def write_csv(path: Path, rows: List[Dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fields = [
        "bundle_path",
        "total_records",
        *ORIGIN_COLUMNS,
        "analyzer_internal_fraction",
        "analyzer_backed_fraction",
        "source_derived_fraction",
        "native_claim_supported",
    ]
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="+", type=Path, help="Bundle files or directories to scan")
    parser.add_argument("--output-json", type=Path)
    parser.add_argument("--output-csv", type=Path)
    parser.add_argument(
        "--minimum-native-fraction",
        type=float,
        default=None,
        help="Exit non-zero if the aggregate analyzer-internal fraction is below this value",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    bundle_paths = discover_bundles(args.inputs)
    if not bundle_paths:
        print("No evidence_bundle.json files found.", file=sys.stderr)
        return 2

    rows = [audit_bundle(path) for path in bundle_paths]
    summary = aggregate(rows)
    result = {"summary": summary, "bundles": rows}

    if args.output_json:
        args.output_json.parent.mkdir(parents=True, exist_ok=True)
        args.output_json.write_text(
            json.dumps(result, indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8",
        )
    if args.output_csv:
        write_csv(args.output_csv, rows)

    print(json.dumps(summary, indent=2, ensure_ascii=False))
    threshold = args.minimum_native_fraction
    if threshold is not None and summary["analyzer_internal_fraction"] < threshold:
        print(
            "Analyzer-internal evidence fraction "
            f"{summary['analyzer_internal_fraction']:.4f} is below required {threshold:.4f}.",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
