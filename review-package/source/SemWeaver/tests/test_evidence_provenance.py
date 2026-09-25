import unittest

from src.evidence_provenance import (
    EVIDENCE_ORIGIN_ANALYZER_INTERNAL,
    EVIDENCE_ORIGIN_ANALYZER_OUTPUT,
    EVIDENCE_ORIGIN_BEHAVIORAL,
    EVIDENCE_ORIGIN_MANUAL,
    EVIDENCE_ORIGIN_SOURCE_DERIVED,
    classify_evidence_origin,
)
from src.core.evidence_schema import (
    EvidenceBundle,
    EvidenceProvenance,
    EvidenceRecord,
)
from src.evidence.normalizer import EvidenceNormalizer


class EvidenceProvenanceTests(unittest.TestCase):
    def test_origin_classification_does_not_treat_analyzer_label_as_native(self):
        self.assertEqual(
            classify_evidence_origin("csa", "patch-diff:path-guard"),
            EVIDENCE_ORIGIN_SOURCE_DERIVED,
        )
        self.assertEqual(
            classify_evidence_origin("csa", "compile-db/source-window:symbolic-state"),
            EVIDENCE_ORIGIN_SOURCE_DERIVED,
        )
        self.assertEqual(
            classify_evidence_origin("csa", "clang-analyzer:debug-cfg"),
            EVIDENCE_ORIGIN_ANALYZER_INTERNAL,
        )
        self.assertEqual(
            classify_evidence_origin("codeql", "codeql-db:call-graph"),
            EVIDENCE_ORIGIN_ANALYZER_INTERNAL,
        )
        self.assertEqual(
            classify_evidence_origin("csa", "csa-validation:result-json"),
            EVIDENCE_ORIGIN_ANALYZER_OUTPUT,
        )
        self.assertEqual(
            classify_evidence_origin("runner", "validation-feedback"),
            EVIDENCE_ORIGIN_BEHAVIORAL,
        )
        self.assertEqual(
            classify_evidence_origin("scripts/backfill_inputs.py", "patch-diff:path-guard"),
            EVIDENCE_ORIGIN_MANUAL,
        )

    def test_bundle_serializes_counts_and_fractions(self):
        bundle = EvidenceBundle(
            records=[
                EvidenceRecord(
                    evidence_id="native",
                    type="path_guard",
                    analyzer="csa",
                    provenance=EvidenceProvenance("csa", "clang-analyzer:debug-cfg"),
                ),
                EvidenceRecord(
                    evidence_id="fallback",
                    type="path_guard",
                    analyzer="csa",
                    provenance=EvidenceProvenance("csa", "source-window:path-guard"),
                ),
            ],
            collected_analyzers=["csa"],
        )

        serialized = bundle.to_dict()
        summary = serialized["provenance_summary"]
        self.assertEqual(summary["total_records"], 2)
        self.assertEqual(summary["analyzer_internal_records"], 1)
        self.assertEqual(summary["source_derived_records"], 1)
        self.assertEqual(summary["analyzer_internal_fraction"], 0.5)
        self.assertEqual(
            serialized["records"][0]["provenance"]["origin"],
            EVIDENCE_ORIGIN_ANALYZER_INTERNAL,
        )

    def test_raw_bundle_preserves_explicit_origin(self):
        raw = {
            "records": [
                {
                    "evidence_id": "legacy",
                    "type": "path_guard",
                    "analyzer": "csa",
                    "provenance": {
                        "tool": "legacy-import",
                        "artifact": "custom-artifact",
                        "confidence": 0.8,
                        "origin": EVIDENCE_ORIGIN_MANUAL,
                    },
                }
            ]
        }
        bundle = EvidenceNormalizer.from_raw_bundle(raw)
        self.assertEqual(bundle.records[0].provenance.origin, EVIDENCE_ORIGIN_MANUAL)
        self.assertEqual(
            bundle.provenance_summary()["origin_counts"],
            {EVIDENCE_ORIGIN_MANUAL: 1},
        )


if __name__ == "__main__":
    unittest.main()
