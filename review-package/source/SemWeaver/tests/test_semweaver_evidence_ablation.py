import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace

import pytest


SCRIPT = (
    Path(__file__).resolve().parents[1]
    / "experiments"
    / "knighter"
    / "experiment"
    / "run_semweaver_treatment_case.py"
)
SPEC = importlib.util.spec_from_file_location("run_semweaver_treatment_case", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC and SPEC.loader
SPEC.loader.exec_module(MODULE)


def test_native_mode_preserves_frozen_bundle_and_origin_gate():
    bundle = {
        "records": [
            {"evidence_id": "native-1", "semantic_payload": {"claim": "guard"}},
            {"evidence_id": "source-1", "semantic_payload": {"claim": "window"}},
        ]
    }

    visible, origin_gate = MODULE.treatment_evidence_view(bundle, "native")

    assert visible is bundle
    assert origin_gate is True


def test_no_internal_mode_removes_all_records_and_disables_origin_gate():
    bundle = {
        "records": [{"evidence_id": "native-1", "semantic_payload": {"secret_claim": "guard"}}],
        "missing_evidence": ["lifetime"],
    }

    visible, origin_gate = MODULE.treatment_evidence_view(bundle, "no_internal")

    assert visible == {"records": [], "missing_evidence": [], "collected_analyzers": []}
    assert origin_gate is False
    assert "secret_claim" not in json.dumps(visible)
    assert bundle["records"]  # The frozen source bundle is not mutated.


def test_unknown_evidence_mode_fails_closed():
    with pytest.raises(ValueError, match="Unsupported evidence mode"):
        MODULE.treatment_evidence_view({}, "source_only")


@pytest.mark.parametrize(
    "mode,expected_count,origin_gate",
    [("native", 1, True), ("no_internal", 0, False)],
)
def test_case_driver_passes_only_selected_evidence_to_agent(
    tmp_path, monkeypatch, mode, expected_count, origin_gate
):
    case = tmp_path / "case_a"
    output = tmp_path / f"output_{mode}"
    evidence = tmp_path / "evidence.json"
    config_path = tmp_path / "config.yaml"
    linux = tmp_path / "linux"
    for directory in (case / "patches", case / "csa", case / "metadata", linux):
        directory.mkdir(parents=True)
    (case / "patches" / "commit.patch").write_text("patch\n", encoding="utf-8")
    (case / "csa" / "SAGenTestChecker.cpp").write_text("checker\n", encoding="utf-8")
    (case / "metadata" / "candidate.json").write_text(
        json.dumps({"commit_id": "0123456789abcdef"}) + "\n", encoding="utf-8"
    )
    evidence.write_text(json.dumps({
        "records": [{
            "evidence_id": "internal-1",
            "type": "path_guard",
            "provenance": {"origin": "analyzer_internal"},
            "semantic_payload": {"claim": "the secret internal guard"},
        }]
    }) + "\n", encoding="utf-8")
    config_path.write_text("llm: {}\n", encoding="utf-8")
    captured = {}

    class FakeAgent:
        def __init__(self, *, config, **_kwargs):
            captured["config"] = config

        def run(self, request):
            captured["request"] = request
            return SimpleNamespace(
                success=False,
                iterations=0,
                compile_attempts=0,
                error_message="no candidate in no-model test",
                final_message="",
                output_path="",
                metadata={},
            )

    monkeypatch.setattr(MODULE, "LangChainRefinementAgent", FakeAgent)
    monkeypatch.setattr(MODULE, "load_config", lambda _path: {
        "llm": {"primary_model": "test-model", "provider": "test", "wire_api": "responses", "generation": {}},
        "agent": {}, "refine": {}, "quality_gates": {},
    })
    monkeypatch.setattr(MODULE, "build_tool_registry", lambda **_kwargs: object())
    monkeypatch.setattr(MODULE, "materialize_frozen_source", lambda **_kwargs: {"demo.c": "frozen"})
    monkeypatch.setattr("sys.argv", [
        "case", "--config", str(config_path), "--case-dir", str(case),
        "--evidence-bundle", str(evidence), "--evidence-dir", str(linux),
        "--output-dir", str(output), "--evidence-mode", mode,
        "--max-iterations", "2", "--max-tokens", "16000",
    ])

    assert MODULE.main() == 2

    request = captured["request"]
    manifest = json.loads((output / "RUN_MANIFEST.json").read_text(encoding="utf-8"))
    assert len(request.evidence_bundle_raw["records"]) == expected_count
    assert request.preload_internal_evidence is origin_gate
    assert request.disable_evidence_requests is True
    assert captured["config"]["quality_gates"]["evidence_provenance"]["enabled"] is origin_gate
    assert captured["config"]["llm"]["generation"]["max_retries"] == 0
    assert manifest["evidence_mode"] == mode
    assert manifest["visible_evidence_record_count"] == expected_count
    assert len(manifest["preloaded_internal_evidence_ids"]) == expected_count
    assert ("ablation" in manifest["method"]) == (mode == "no_internal")
