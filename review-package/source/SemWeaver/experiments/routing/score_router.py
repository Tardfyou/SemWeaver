#!/usr/bin/env python3
"""Score patch-mechanism and evidence-role routing predictions."""

from __future__ import annotations

import argparse
import csv
import json
from collections import Counter
from pathlib import Path
from typing import Dict, Iterable, Set


def role_set(value: str) -> Set[str]:
    return {
        token.strip()
        for token in str(value or "").replace(",", ";").split(";")
        if token.strip()
    }


def truthy(value: str) -> bool:
    return str(value or "").strip().lower() in {"1", "true", "yes"}


def index_rows(path: Path) -> Dict[str, Dict[str, str]]:
    with path.open(encoding="utf-8-sig", newline="") as handle:
        rows = list(csv.DictReader(handle))
    indexed: Dict[str, Dict[str, str]] = {}
    for row in rows:
        case_id = str(row.get("case_id", "") or "").strip()
        if not case_id:
            raise ValueError(f"Missing case_id in {path}")
        if case_id in indexed:
            raise ValueError(f"Duplicate case_id {case_id!r} in {path}")
        indexed[case_id] = row
    return indexed


def safe_ratio(numerator: int, denominator: int) -> float:
    return numerator / denominator if denominator else 0.0


def score(gold: Dict[str, Dict[str, str]], predictions: Dict[str, Dict[str, str]]) -> Dict[str, object]:
    mechanism_correct = 0
    exact_roles = 0
    covered = 0
    abstained = 0
    tp = fp = fn = 0
    per_role: Dict[str, Counter] = {}
    cases = []

    for case_id, gold_row in gold.items():
        prediction = predictions.get(case_id, {})
        is_abstained = not prediction or truthy(prediction.get("abstained", ""))
        gold_mechanism = str(gold_row.get("adjudicated_mechanism", "") or "").strip()
        gold_roles = role_set(gold_row.get("adjudicated_roles", ""))
        predicted_mechanism = str(prediction.get("predicted_mechanism", "") or "").strip()
        predicted_roles = role_set(prediction.get("predicted_roles", ""))

        if is_abstained:
            abstained += 1
        else:
            covered += 1
            mechanism_correct += int(predicted_mechanism == gold_mechanism)
            exact_roles += int(predicted_roles == gold_roles)

        all_roles = gold_roles | predicted_roles
        for role in all_roles:
            counts = per_role.setdefault(role, Counter())
            if role in gold_roles and role in predicted_roles:
                counts["tp"] += 1
                tp += 1
            elif role in predicted_roles:
                counts["fp"] += 1
                fp += 1
            else:
                counts["fn"] += 1
                fn += 1

        cases.append(
            {
                "case_id": case_id,
                "abstained": is_abstained,
                "mechanism_correct": (not is_abstained and predicted_mechanism == gold_mechanism),
                "roles_exact": (not is_abstained and predicted_roles == gold_roles),
                "gold_mechanism": gold_mechanism,
                "predicted_mechanism": predicted_mechanism,
                "gold_roles": sorted(gold_roles),
                "predicted_roles": sorted(predicted_roles),
            }
        )

    precision = safe_ratio(tp, tp + fp)
    recall = safe_ratio(tp, tp + fn)
    f1 = safe_ratio(2 * precision * recall, precision + recall)
    return {
        "summary": {
            "gold_cases": len(gold),
            "prediction_rows": len(predictions),
            "covered_cases": covered,
            "coverage": safe_ratio(covered, len(gold)),
            "abstained_cases": abstained,
            "abstention_rate": safe_ratio(abstained, len(gold)),
            "missing_predictions": len(set(gold) - set(predictions)),
            "unexpected_predictions": len(set(predictions) - set(gold)),
            "mechanism_accuracy_on_covered": safe_ratio(mechanism_correct, covered),
            "exact_role_set_match_on_covered": safe_ratio(exact_roles, covered),
            "role_micro_precision": precision,
            "role_micro_recall": recall,
            "role_micro_f1": f1,
        },
        "per_role": {role: dict(sorted(counts.items())) for role, counts in sorted(per_role.items())},
        "cases": cases,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gold", required=True, type=Path)
    parser.add_argument("--predictions", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    report = score(index_rows(args.gold), index_rows(args.predictions))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps(report["summary"], indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
