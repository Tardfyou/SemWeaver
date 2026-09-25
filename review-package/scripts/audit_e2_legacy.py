#!/usr/bin/env python3
"""Screen the historical E2 table for obvious manual provenance.

This is not a regenerated E2 result: path markers cannot prove that unmarked
rows were fully automatic. Use the output only to audit why the old 20-row
summary cannot support an automatic cross-backend effectiveness claim.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from collections import Counter
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv_file", type=Path)
    parser.add_argument("--output-json", type=Path)
    args = parser.parse_args()
    with args.csv_file.open(newline="", encoding="utf-8-sig") as handle:
        rows = list(csv.DictReader(handle))
    assert len(rows) == 20
    marked = []
    unmarked = []
    for row in rows:
        paths = (row.get("input_root") or "") + "/" + (row.get("report_path") or "")
        (marked if "_manual" in paths else unmarked).append(row)

    def summary(items: list[dict[str, str]]) -> dict:
        return {
            "rows": len(items),
            "by_backend": dict(Counter(row["analyzer"] for row in items)),
            "baseline_pds": sum(row["baseline_pds"] == "TRUE" for row in items),
            "final_pds": sum(row["refine_pds"] == "TRUE" for row in items),
            "zero_model_calls": sum(int(row["llm_calls"] or 0) == 0 for row in items),
            "transitions": {
                f"{before}->{after}": count
                for (before, after), count in Counter(
                    (row["baseline_pds"], row["refine_pds"]) for row in items
                ).items()
            },
        }

    result = {
        "source_csv_sha256": hashlib.sha256(args.csv_file.read_bytes()).hexdigest(),
        "analysis_script_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "scope": "historical E2 table provenance screen, not automatic efficacy",
        "all_rows": summary(rows),
        "manual_path_marked": summary(marked),
        "unmarked_candidates_not_yet_verified": summary(unmarked),
        "manual_path_case_backend": sorted(
            [row["sample_id"], row["analyzer"]] for row in marked
        ),
        "interpretation": (
            "The source table's baseline PDS count differs from the old figure's "
            "8/20. Manual path markers and zero-call rows prevent treating the "
            "old final count as an automatic treatment effect."
        ),
    }
    rendered = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.output_json:
        args.output_json.parent.mkdir(parents=True, exist_ok=True)
        args.output_json.write_text(rendered, encoding="utf-8")
    print(rendered, end="")


if __name__ == "__main__":
    main()
