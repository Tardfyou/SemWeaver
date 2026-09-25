#!/usr/bin/env python3
"""Detect manual intervention and ambiguous provenance in result tables."""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from pathlib import Path
from typing import Dict, Iterable, List


MANUAL_PATH_MARKER = re.compile(
    r"(?:^|[/_\-])manual(?:$|[/_\-])|manual[_-]?skip|human[_-]?assisted",
    re.IGNORECASE,
)
MANUAL_TEXT_MARKER = re.compile(
    r"manual[_-]?validated|manual(?:ly)?\s+(?:repair|repaired|completion|completed|validation|validated|checker)|human[_-]?assisted",
    re.IGNORECASE,
)
NEGATED_MANUAL_TEXT = re.compile(
    r"\bno(?:\s+further)?\s+manual\s+(?:repair|completion|validation)\b[^.;]*",
    re.IGNORECASE,
)
INTERVENTION_FIELDS = (
    "intervention",
    "intervention_type",
    "execution_mode",
    "automation_status",
)
IDENTIFIER_FIELDS = ("case_id", "sample_id", "commit_id", "analyzer")
PATH_FIELDS = ("input_root", "report_path", "run_dir", "checker_path", "final_report_path")
TEXT_FIELDS = ("status", "refine_success", "notes", "validation_feedback", "error_message")


def row_id(row: Dict[str, str], index: int) -> str:
    parts = [str(row.get(field, "") or "").strip() for field in IDENTIFIER_FIELDS]
    compact = [part for part in parts if part]
    return ":".join(compact) if compact else f"row-{index}"


def audit_rows(rows: Iterable[Dict[str, str]]) -> List[Dict[str, object]]:
    findings: List[Dict[str, object]] = []
    for index, row in enumerate(rows, start=2):
        reasons: List[str] = []
        explicit = next(
            (
                str(row.get(field, "") or "").strip().lower()
                for field in INTERVENTION_FIELDS
                if str(row.get(field, "") or "").strip()
            ),
            "",
        )
        if explicit and explicit not in {"automatic", "auto", "none"}:
            reasons.append(f"explicit intervention={explicit}")

        for field in PATH_FIELDS:
            text = str(row.get(field, "") or "")
            if text and MANUAL_PATH_MARKER.search(text):
                reasons.append(f"{field} contains manual-intervention marker")
        for field in TEXT_FIELDS:
            text = str(row.get(field, "") or "")
            positive_text = NEGATED_MANUAL_TEXT.sub("", text)
            if positive_text and MANUAL_TEXT_MARKER.search(positive_text):
                reasons.append(f"{field} contains manual-intervention marker")

        if reasons:
            findings.append(
                {
                    "row_number": index,
                    "row_id": row_id(row, index),
                    "reasons": sorted(set(reasons)),
                }
            )
    return findings


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tables", nargs="+", type=Path)
    parser.add_argument("--output-json", type=Path)
    parser.add_argument(
        "--require-automatic",
        action="store_true",
        help="Exit non-zero if any row has manual or human-assisted provenance",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    table_reports = []
    total_rows = 0
    total_flagged = 0
    for path in args.tables:
        resolved = path.expanduser().resolve()
        with resolved.open(encoding="utf-8-sig", newline="") as handle:
            rows = list(csv.DictReader(handle))
        findings = audit_rows(rows)
        total_rows += len(rows)
        total_flagged += len(findings)
        table_reports.append(
            {
                "table": str(resolved),
                "row_count": len(rows),
                "flagged_count": len(findings),
                "findings": findings,
            }
        )

    report = {
        "summary": {
            "table_count": len(table_reports),
            "row_count": total_rows,
            "flagged_count": total_flagged,
            "automatic_only": total_flagged == 0,
        },
        "tables": table_reports,
    }
    if args.output_json:
        args.output_json.parent.mkdir(parents=True, exist_ok=True)
        args.output_json.write_text(
            json.dumps(report, indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8",
        )
    print(json.dumps(report, indent=2, ensure_ascii=False))
    return 1 if args.require_automatic and total_flagged else 0


if __name__ == "__main__":
    raise SystemExit(main())
