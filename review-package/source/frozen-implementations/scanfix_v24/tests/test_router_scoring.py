import importlib.util
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "experiments" / "routing" / "score_router.py"
SPEC = importlib.util.spec_from_file_location("score_router", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


class RouterScoringTests(unittest.TestCase):
    def test_scores_covered_and_abstained_cases(self):
        gold = {
            "a": {"adjudicated_mechanism": "cleanup", "adjudicated_roles": "guard;lifetime"},
            "b": {"adjudicated_mechanism": "bounds", "adjudicated_roles": "flow;guard"},
        }
        predictions = {
            "a": {"predicted_mechanism": "cleanup", "predicted_roles": "guard;lifetime", "abstained": "false"},
            "b": {"predicted_mechanism": "", "predicted_roles": "", "abstained": "true"},
        }
        report = MODULE.score(gold, predictions)
        summary = report["summary"]
        self.assertEqual(summary["covered_cases"], 1)
        self.assertEqual(summary["abstained_cases"], 1)
        self.assertEqual(summary["mechanism_accuracy_on_covered"], 1.0)
        self.assertEqual(summary["exact_role_set_match_on_covered"], 1.0)


if __name__ == "__main__":
    unittest.main()
