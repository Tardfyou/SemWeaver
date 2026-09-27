#!/usr/bin/env python3
"""Reaggregate corrected evidence without overwriting or hiding the old runs."""
from __future__ import annotations
import argparse
import csv
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent
DATA = ROOT / "LLM-Native/artifacts/fse_revision"
ORIGINAL_DIGESTS = {}
G08 = "G08_51df94767836_Memory_Leak"
QUEUES = ["scanfix_v24_g08_e3_queue_r2", "scanfix_v24_g08_e4_queue_r2", "scanfix_v24_g08_matched_queue", "scanfix_v25_remaining_gpt_queue", "scanfix_v25_remaining_glm_queue", "scanfix_v26_g08_e3_queue_r3", "scanfix_v26_provider_corrections_queue"]
PORTFOLIOS = {
    "gpt": "e4_all39_gpt6luna_high_portfolio_v19",
    "glm53": "e4_all39_glm53_mixed_wire_portfolio_v20",
    "flash": "e4_all39_glmflash_mixed_wire_portfolio_v20",
    "e3_native": "e3_all39_gpt6luna_high_native_v13_portfolio",
    "e3_no_internal": "e3_all39_gpt6luna_high_no_internal_v13_portfolio",
}


def load(path):
    return json.loads(Path(path).read_text())


def sha(path):
    actual = hashlib.sha256(Path(path).read_bytes()).hexdigest()
    mapping = ORIGINAL_DIGESTS.get(str(Path(path).resolve()))
    if mapping:
        assert actual == mapping["redacted_sha256"], f"Redacted file drift: {path}"
        return mapping["original_sha256"]
    return actual


def relative(path):
    return str(Path(path).resolve().relative_to(DATA.resolve()))


def metrics(rows):
    assert all(isinstance(r["vulnerable_alerts"], int) and isinstance(r["fixed_alerts"], int) for r in rows)
    tp = sum(r["vulnerable_alerts"] > 0 for r in rows)
    fp = sum(r["fixed_alerts"] > 0 for r in rows)
    fn = len(rows) - tp
    pds = sum(r["vulnerable_alerts"] > 0 and r["fixed_alerts"] == 0 for r in rows)
    return {"cases": len(rows), "TP": tp, "FP": fp, "FN": fn, "TN": len(rows) - fp, "PDS": pds, "precision": tp / (tp + fp) if tp + fp else 0, "recall": tp / len(rows) if rows else 0, "F1": 2 * tp / (2 * tp + fp + fn) if 2 * tp + fp + fn else 0, "fixed_object_alerts": sum(r["fixed_alerts"] for r in rows)}


def result_hashes(value):
    found = []
    if isinstance(value, dict):
        for key, item in value.items():
            if key == "batch_result_sha256":
                found.append(item)
            else:
                found.extend(result_hashes(item))
    elif isinstance(value, list):
        for item in value:
            found.extend(result_hashes(item))
    return found


class Aggregator:
    def __init__(self):
        self.replays = {}
        for path in sorted(DATA.glob("scanfix_v24_replay_shard*/RESULT.json")):
            result = load(path)
            assert result["completed"] == result["requested"]
            for row in result["rows"]:
                assert row["old_result"] not in self.replays
                assert sha(DATA / row["old_result"]) == row["old_result_sha256"]
                assert sha(DATA / row["result"]) == row["result_sha256"]
                self.replays[row["old_result"]] = row
        assert len(self.replays) == 55
        self.replacements = {}
        for group in ("gpt", "glm"):
            for row in load(DATA / f"scanfix_v25_remaining_{group}_queue/RESULT.json")["rows"]:
                self.replacements[(row["old_batch"], row["case_id"])] = DATA / Path(row["result_path"]).parent.name
        for row in load(DATA / "scanfix_v26_provider_corrections_queue/RESULT.json")["rows"]:
            self.replacements[(row["old_batch"], row["case_id"])] = DATA / Path(row["result_path"]).parent.name
        self.index = {sha(p): p.parent for p in DATA.glob("*/BATCH_RESULT.json")}
        screen = load(DATA / "generate_only_screen_v1/SCREEN_RESULT.json")
        self.start = {}
        for row in screen["rows"]:
            old = DATA / "generate_only_screen_v1" / row["case_id"] / "RESULT.json"
            actual = self.validation_path(old)
            verdict = load(actual)
            assert verdict["execution_valid"] and verdict["candidate_sha256"] == row["checker_sha256"]
            self.start[row["case_id"]] = {"case_id": row["case_id"], "candidate_sha256": row["checker_sha256"], "vulnerable_alerts": verdict["vulnerable_alerts"], "fixed_alerts": verdict["fixed_alerts"], "validation_path": relative(actual), "validation_sha256": sha(actual)}
        assert len(self.start) == 39
        self.precision = {case for case, r in self.start.items() if r["vulnerable_alerts"] > 0 and r["fixed_alerts"] > 0}
        assert len(self.precision) == 15

    def validation_path(self, old):
        row = self.replays.get(relative(old))
        return DATA / row["result"] if row else old

    def case(self, batch, case):
        batch = self.replacements.get((batch.name, case), batch)
        result_path = batch / "BATCH_RESULT.json"
        batch_result = load(result_path)
        row = next(r for r in batch_result["rows"] if r["case_id"] == case)
        if row["llm_calls"] == 0 and row.get("outcome") != "evidence_abstention":
            raise ValueError(f"Unexplained zero-call cell is not a completed experiment: {batch.name}/{case}")
        initial = self.start[case]
        chosen = None
        attempts = []
        prefix_calls = 0
        for attempt in row.get("attempt_rows", []):
            prefix_calls += attempt["llm_calls"]
            directory = batch / case / f"attempt-{attempt['attempt']:02d}"
            events = directory / "run_events.jsonl"
            if events.is_file() and any(load_event.get("event") == "agent_exception" for load_event in (json.loads(line) for line in events.read_text().splitlines() if line.strip())):
                raise ValueError(f"Unscored agent exception reached aggregation: {batch.name}/{case}/{attempt['attempt']}")
            old = directory / "frozen_validation/RESULT.json"
            if not old.is_file():
                attempts.append({"attempt": attempt["attempt"], "outcome": attempt["outcome"], "validation_path": None})
                continue
            actual = self.validation_path(old)
            verdict = load(actual)
            assert verdict["candidate_sha256"] == sha(directory / "SAGenTestChecker.cpp")
            item = {"attempt": attempt["attempt"], "validation_path": relative(actual), "validation_sha256": sha(actual), "candidate_path": relative(directory / "SAGenTestChecker.cpp"), "candidate_sha256": verdict["candidate_sha256"], "execution_valid": verdict["execution_valid"], "vulnerable_alerts": verdict["vulnerable_alerts"], "fixed_alerts": verdict["fixed_alerts"], "pds": verdict["pds"], "retained_prefix_calls": prefix_calls}
            attempts.append(item)
            if verdict["execution_valid"] and verdict["pds"]:
                assert verdict["vulnerable_alerts"] > 0 and verdict["fixed_alerts"] == 0
                chosen = item
                break
            if actual != old:
                previous = load(old)
                same_feedback = all(previous.get(k) == verdict.get(k) for k in ("execution_valid", "vulnerable_alerts", "fixed_alerts", "pds"))
                if not same_feedback and attempt != row["attempt_rows"][-1]:
                    raise ValueError(f"Cannot reuse a continuation after divergent feedback: {batch.name}/{case}/{attempt['attempt']}")
        valid = [r for r in attempts if r.get("execution_valid")]
        preserving = [r for r in valid if r["vulnerable_alerts"] > 0]
        best = min(preserving, key=lambda r: (r["fixed_alerts"], r["attempt"])) if preserving else None
        selected = chosen or initial
        return {"case_id": case, "stratum": "precision" if case in self.precision else "recovery", "vulnerable_alerts": selected["vulnerable_alerts"], "fixed_alerts": selected["fixed_alerts"], "accepted_pds": chosen is not None, "selected_candidate_sha256": selected["candidate_sha256"], "validation_path": selected["validation_path"], "validation_sha256": selected["validation_sha256"], "source_batch": batch.name, "source_batch_result_sha256": sha(result_path), "actual_model_calls": row["llm_calls"], "accepted_prefix_calls": chosen["retained_prefix_calls"] if chosen else None, "call_cap": row.get("call_cap"), "candidate_attempts": attempts, "best_hit_preserving_candidate": best}

    def g08(self, key, repeat=None):
        if key == "e3_no_internal" and repeat == 2:
            name = "scanfix_v26_g08_e3_gpt_no_internal_precision_rep02_r3"
        elif key.startswith("e3_"):
            mode = key.removeprefix("e3_")
            suffix = "full" if repeat is None else f"rep{repeat:02d}"
            name = f"scanfix_v24_g08_e3_gpt_{mode}_precision_{suffix}_r2"
        elif key.startswith("matched_"):
            name = "scanfix_v24_g08_matched_" + key.removeprefix("matched_")
        else:
            model = {"gpt": "gpt-6-luna", "glm53": "glm-5.3", "flash": "glm-5.3-flash"}[key]
            name = f"scanfix_v24_g08_e4_{model}_precision_rep{repeat or 1:02d}_r2"
        return self.case(DATA / name, G08)

    def portfolio(self, key):
        original = load(DATA / PORTFOLIOS[key] / "RESULT.json")
        rows = {}
        for digest in result_hashes(original["batches"]):
            batch = self.index[digest]
            for row in load(batch / "BATCH_RESULT.json")["rows"]:
                case = row["case_id"]
                assert case not in rows
                rows[case] = self.g08(key) if case == G08 else self.case(batch, case)
        assert set(rows) == set(self.start)
        return {"metrics": metrics(list(rows.values())), "rows": list(rows.values())}

    def repeated(self, portfolios):
        precision = load(DATA / "e3_e4_repeat_subset_v10/PRECISION_COHORT_MANIFEST.json")["selected_rows"]
        recovery = load(DATA / "e3_e4_repeat_subset_v10/RECOVERY_COHORT_MANIFEST.json")["selected_rows"]
        ids = {r["case_id"] for r in precision + recovery}
        assert len(ids) == 12 and len(ids & self.precision) == 5
        results = {}
        for key in ("e3_native", "e3_no_internal", "gpt", "glm53", "flash"):
            cells = []
            for rep in (1, 2, 3):
                if not key.startswith("e3_") and rep == 1:
                    rows = [r for r in portfolios[key]["rows"] if r["case_id"] in ids]
                else:
                    rows = []
                    for stratum in ("precision", "recovery"):
                        if key.startswith("e3_"):
                            mode = key.removeprefix("e3_")
                            name = f"e3_repeat12_gpt6luna_high_{mode}_{stratum}_v13_rep{rep:02d}"
                        elif key == "gpt":
                            name = f"e4_repeat12_gpt6luna_high_{stratum}_rep{rep:02d}"
                        else:
                            model = "glm53" if key == "glm53" else "glmflash"
                            suffix = "_clean" if stratum == "recovery" or (model == "glmflash" and rep == 3) else ""
                            name = f"e4_repeat12_{model}_anthropic_v22_{stratum}_rep{rep:02d}{suffix}"
                        for old in load(DATA / name / "BATCH_RESULT.json")["rows"]:
                            case = old["case_id"]
                            rows.append(self.g08(key, rep) if case == G08 else self.case(DATA / name, case))
                assert {r["case_id"] for r in rows} == ids
                cells.append({"repeat": rep, "precision": metrics([r for r in rows if r["case_id"] in self.precision]), "recovery": metrics([r for r in rows if r["case_id"] not in self.precision]), "rows": rows})
            results[key] = cells
        return results

    def baseline(self):
        old = load(DATA / "all39_knighter_system_v10/RESULT.json")
        strict = load(DATA / "scanfix_v24_knighter_g08_strict/STRICT_RESULT.json")
        assert strict["unscored"] == 0 and strict["strict_pds"] == 0
        rows = []
        for row in old["rows"]:
            row = dict(row)
            if row["case_id"] == G08:
                row.update(self.start[G08])
                row.update({"model_calls": strict["baseline_model_calls"], "refinement_entrypoint": "actual_report_refinement_after_corrected_screen", "refinement_applicable": True, "adopted_strict_pds": False})
            if row.get("adopted_strict_pds"):
                proof = DATA / "generate_only_knighter_gpt6luna_high_v8_strict_v1" / row["case_id"] / "RESULT.json"
                verified = load(proof)
                assert verified["execution_valid"] and verified["pds"] and verified["candidate_sha256"] == row["candidate_sha256"]
                row["validation_path"] = relative(proof)
                row["validation_sha256"] = sha(proof)
            else:
                row["validation_path"] = self.start[row["case_id"]]["validation_path"]
                row["validation_sha256"] = self.start[row["case_id"]]["validation_sha256"]
            rows.append(row)
        return {"metrics": metrics(rows), "rows": rows}

    def matched(self, baseline):
        result = {"knighter": {"metrics": metrics([r for r in baseline["rows"] if r["case_id"] in self.precision]), "actual_model_calls": sum(r["model_calls"] for r in baseline["rows"] if r["case_id"] in self.precision)}}
        for mode in ("native", "no_internal"):
            old = load(DATA / f"e1_matched14_gpt6luna_high_{mode}_comparison_v20/RESULT.json")
            batch = self.index[old["semweaver_batch_result_sha256"]]
            rows = [self.case(batch, r["case_id"]) for r in load(batch / "BATCH_RESULT.json")["rows"]]
            rows.append(self.g08("matched_" + mode))
            assert {r["case_id"] for r in rows} == self.precision
            result[mode] = {"metrics": metrics(rows), "actual_model_calls": sum(r["actual_model_calls"] for r in rows), "rows": rows}
        return result


def main():
    global DATA, ORIGINAL_DIGESTS
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--data-root", type=Path, default=DATA)
    parser.add_argument("--redactions", type=Path, help="Packaged REDACTIONS.json for original-to-redacted digest reconciliation")
    args = parser.parse_args()
    DATA = args.data_root.resolve()
    if args.redactions:
        redactions = args.redactions.resolve()
        ORIGINAL_DIGESTS = {str((redactions.parent / row["path"]).resolve()): row for row in load(redactions)["files"]}
    pending = []
    for name in QUEUES:
        p = DATA / name
        if not (p / "RESULT.json").exists():
            pending.append({"queue": name, "interrupted": (p / "INTERRUPTED.json").exists(), "progress": load(p / "PROGRESS.json") if (p / "PROGRESS.json").exists() else None})
    if pending:
        print(json.dumps({"status": "pending", "queues": [{"queue": r["queue"], "interrupted": r["interrupted"], "completed": (r["progress"] or {}).get("completed"), "requested": (r["progress"] or {}).get("requested")} for r in pending]}, indent=2))
        return 3
    if not args.output_dir:
        print(json.dumps({"status": "queues_complete_aggregation_not_run"})); return 0
    output = args.output_dir.resolve()
    if output.exists():
        raise FileExistsError(output)
    agg = Aggregator()
    portfolios = {key: agg.portfolio(key) for key in PORTFOLIOS}
    baseline = agg.baseline()
    result = {"schema_version": 1, "method": "scan_integrity_corrected_frozen_cohort", "starting": {"metrics": metrics(list(agg.start.values())), "rows": list(agg.start.values())}, "knighter": baseline, "portfolios": portfolios, "matched15": agg.matched(baseline), "repeated12": agg.repeated(portfolios), "correction_map": agg.replays, "inference_boundary": "Same 39 subjects; corrected starting strata 15/24 and retained repeat subset 5/7. Fresh runs replace wrong-feedback chains; corrected frozen prefixes stop at their earliest PDS, never select a favorable later candidate. Actual consumed calls and accepted-prefix calls remain distinct."}
    output.mkdir(parents=True)
    (output / "RESULT.json").write_text(json.dumps(result, indent=2) + "\n")
    table = []
    for method, block in {"unmodified": result["starting"], "knighter": baseline, **portfolios}.items():
        table.append({"method": method, **block["metrics"]})
    with (output / "SUMMARY.csv").open("w") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(table[0]));writer.writeheader();writer.writerows(table)
    print(json.dumps({r["method"]: r for r in table}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
