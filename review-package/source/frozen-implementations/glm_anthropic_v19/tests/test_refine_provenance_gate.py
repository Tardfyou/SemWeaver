import unittest

from src.refine.agent import LangChainRefinementAgent
from src.refine.models import RefinementRequest


def request_with(records):
    return RefinementRequest(
        analyzer="csa",
        patch_path="fix.patch",
        work_dir=".",
        target_path="Checker.cpp",
        checker_name="Checker",
        evidence_bundle_raw={"records": records},
    )


class RefineProvenanceGateTests(unittest.TestCase):
    def setUp(self):
        self.agent = LangChainRefinementAgent(
            config={
                "quality_gates": {
                    "evidence_provenance": {
                        "enabled": True,
                        "required_origins": ["analyzer_internal"],
                        "minimum_records": 1,
                    }
                }
            },
            llm_override=object(),
        )

    def test_gate_rejects_source_derived_record(self):
        ok, reason, metrics = self.agent._evidence_provenance_gate(
            request_with(
                [
                    {
                        "evidence_id": "fallback",
                        "type": "path_guard",
                        "analyzer": "csa",
                        "provenance": {
                            "tool": "csa",
                            "artifact": "source-window:path-guard",
                        },
                    }
                ]
            )
        )
        self.assertFalse(ok)
        self.assertIn("ABSTAIN", reason)
        self.assertEqual(metrics["analyzer_internal_records"], 0)

    def test_gate_accepts_analyzer_internal_record(self):
        ok, reason, metrics = self.agent._evidence_provenance_gate(
            request_with(
                [
                    {
                        "evidence_id": "native",
                        "type": "path_guard",
                        "analyzer": "csa",
                        "provenance": {
                            "tool": "csa",
                            "artifact": "clang-analyzer:debug-cfg",
                        },
                    }
                ]
            )
        )
        self.assertTrue(ok)
        self.assertEqual(reason, "")
        self.assertEqual(metrics["analyzer_internal_records"], 1)

    def test_run_abstains_before_tools_or_model(self):
        result = self.agent.run(request_with([]))
        self.assertFalse(result.success)
        self.assertTrue(result.metadata["abstained"])
        self.assertEqual(result.iterations, 0)


if __name__ == "__main__":
    unittest.main()
