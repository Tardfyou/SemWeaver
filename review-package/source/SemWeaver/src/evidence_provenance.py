"""Stable evidence-origin taxonomy shared by runtime and audit tooling."""

from __future__ import annotations

from typing import Any, Dict, Iterable


EVIDENCE_ORIGIN_ANALYZER_INTERNAL = "analyzer_internal"
EVIDENCE_ORIGIN_ANALYZER_OUTPUT = "analyzer_output"
EVIDENCE_ORIGIN_SOURCE_DERIVED = "source_derived"
EVIDENCE_ORIGIN_BEHAVIORAL = "behavioral_feedback"
EVIDENCE_ORIGIN_MANUAL = "manual_or_backfilled"
EVIDENCE_ORIGIN_UNKNOWN = "unknown"

EVIDENCE_ORIGINS = (
    EVIDENCE_ORIGIN_ANALYZER_INTERNAL,
    EVIDENCE_ORIGIN_ANALYZER_OUTPUT,
    EVIDENCE_ORIGIN_SOURCE_DERIVED,
    EVIDENCE_ORIGIN_BEHAVIORAL,
    EVIDENCE_ORIGIN_MANUAL,
    EVIDENCE_ORIGIN_UNKNOWN,
)


def classify_evidence_origin(tool: str, artifact: str) -> str:
    """Classify provenance without conflating analyzer labels with native facts.

    A record is analyzer-internal only when its persisted artifact identifies a
    runtime representation produced by the analyzer (for example, CSA CFG/state
    output or a live CodeQL database query).  Merely setting ``analyzer=csa`` or
    ``tool=csa`` is not sufficient: patch-derived and source-window fallbacks
    remain source-derived evidence.
    """

    normalized_tool = str(tool or "").strip().lower()
    normalized_artifact = str(artifact or "").strip().lower()
    combined = f"{normalized_tool} {normalized_artifact}"

    if any(marker in combined for marker in ("manual", "backfill", "backfilled")):
        return EVIDENCE_ORIGIN_MANUAL
    if "patch-analysis" in normalized_tool:
        return EVIDENCE_ORIGIN_SOURCE_DERIVED
    if "patch_local_result.json" in normalized_artifact or "patch-local-result.json" in normalized_artifact:
        return EVIDENCE_ORIGIN_BEHAVIORAL
    if "run_e2_patch_local_audit" in normalized_tool:
        return EVIDENCE_ORIGIN_BEHAVIORAL
    if "knighter" in normalized_tool and any(
        marker in normalized_artifact for marker in ("report", "triage", "result")
    ):
        return EVIDENCE_ORIGIN_ANALYZER_OUTPUT
    if normalized_artifact.startswith(("clang-analyzer:", "codeql-db:")):
        return EVIDENCE_ORIGIN_ANALYZER_INTERNAL
    if normalized_artifact.startswith(("csa-validation:", "codeql-validation:")):
        return EVIDENCE_ORIGIN_ANALYZER_OUTPUT
    if normalized_artifact.startswith(
        ("patch-diff:", "source-window:", "compile-db/source-window:")
    ):
        return EVIDENCE_ORIGIN_SOURCE_DERIVED
    if any(
        marker in normalized_artifact
        for marker in ("validation-outcome", "validation_feedback", "validation-feedback")
    ):
        return EVIDENCE_ORIGIN_BEHAVIORAL
    return EVIDENCE_ORIGIN_UNKNOWN


def summarize_raw_evidence_provenance(
    records: Iterable[Dict[str, Any]],
    *,
    analyzer: str = "",
) -> Dict[str, Any]:
    """Summarize raw JSON records without importing the core schema package."""

    analyzer_id = str(analyzer or "").strip()
    counts = {origin: 0 for origin in EVIDENCE_ORIGINS}
    total = 0
    for record in records or []:
        if analyzer_id and str(record.get("analyzer", "") or "").strip() != analyzer_id:
            continue
        provenance = record.get("provenance", {}) or {}
        explicit = str(provenance.get("origin", "") or "").strip()
        origin = explicit if explicit in counts else classify_evidence_origin(
            str(provenance.get("tool", "") or ""),
            str(provenance.get("artifact", "") or ""),
        )
        counts[origin] += 1
        total += 1

    internal = counts[EVIDENCE_ORIGIN_ANALYZER_INTERNAL]
    analyzer_backed = internal + counts[EVIDENCE_ORIGIN_ANALYZER_OUTPUT]
    source_derived = counts[EVIDENCE_ORIGIN_SOURCE_DERIVED]
    return {
        "total_records": total,
        "origin_counts": {origin: count for origin, count in counts.items() if count},
        "analyzer_internal_records": internal,
        "analyzer_internal_fraction": (internal / total) if total else 0.0,
        "analyzer_backed_records": analyzer_backed,
        "analyzer_backed_fraction": (analyzer_backed / total) if total else 0.0,
        "source_derived_records": source_derived,
        "source_derived_fraction": (source_derived / total) if total else 0.0,
    }
