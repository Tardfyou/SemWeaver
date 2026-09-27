import importlib.util
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "experiments" / "audit_result_integrity.py"
SPEC = importlib.util.spec_from_file_location("audit_result_integrity", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


class ResultIntegrityAuditTests(unittest.TestCase):
    def test_flags_manual_status_notes_and_paths(self):
        rows = [
            {"case_id": "a", "refine_success": "manual_validated"},
            {"case_id": "b", "notes": "Manual repair applied"},
            {"case_id": "c", "report_path": "runs/c/refine_manual/final.json"},
        ]
        findings = MODULE.audit_rows(rows)
        self.assertEqual(len(findings), 3)

    def test_accepts_explicit_automatic_rows(self):
        rows = [
            {
                "case_id": "a",
                "intervention": "automatic",
                "refine_success": "TRUE",
                "report_path": "runs/a/refine/final.json",
            }
        ]
        self.assertEqual(MODULE.audit_rows(rows), [])

    def test_ignores_manual_labels_and_negated_manual_work(self):
        rows = [
            {
                "case_id": "a",
                "knighter_candidate_group": "local_triage_manual_fp",
                "validation_feedback": "Automatic failure; no further manual repair is applied.",
            }
        ]
        self.assertEqual(MODULE.audit_rows(rows), [])


if __name__ == "__main__":
    unittest.main()
