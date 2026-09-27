import json
import sys
from pathlib import Path

import pytest

import run_e2_codeql_paired_loop as loop


def _write(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


@pytest.mark.parametrize("fixed_after,expected_success", [(0, True), (4, False)])
def test_automatic_loop_only_succeeds_after_independent_pair(
    tmp_path, monkeypatch, fixed_after, expected_success,
):
    initial = tmp_path / "query" / "Detector.ql"
    _write(initial, "import cpp\nselect 1\n")
    _write(initial.parent / "qlpack.yml", 'name: test\ndependencies:\n  codeql/cpp-all: "*"\n')
    patch = tmp_path / "fix.patch"
    _write(patch, "diff --git a/x.c b/x.c\n")
    codeql = tmp_path / "codeql"
    _write(codeql, "binary placeholder\n")
    vulnerable = tmp_path / "vulnerable_db"
    fixed = tmp_path / "fixed_db"
    _write(vulnerable / "codeql-database.yml", "vulnerable\n")
    _write(fixed / "codeql-database.yml", "fixed\n")
    output = tmp_path / "run"
    calls = []

    def fake_launch(command, _stdout, _stderr):
        calls.append(command)
        destination = Path(command[command.index("--output-dir") + 1])
        destination.mkdir(parents=True, exist_ok=True)
        if command[1].endswith("validate_e2_codeql_pair.py"):
            query = Path(command[command.index("--query") + 1])
            improved = query != initial
            loop.write_json(destination / "RESULT.json", {
                "query_sha256": loop.sha256(query),
                "execution_valid": True,
                "vulnerable_rows": 1 if improved else 0,
                "fixed_rows": fixed_after if improved else 0,
                "pds": improved and fixed_after == 0,
            })
            return 0
        assert command[1].endswith("run_e2_codeql_development.py")
        source_query = Path(command[command.index("--initial-query") + 1])
        _write(destination / source_query.name, source_query.read_text() + "// model edit\n")
        loop.write_json(destination / "RUN_MANIFEST.json", {
            "agent_success": True, "llm_usage": {"call_count": 1},
        })
        return 0

    monkeypatch.setattr(loop, "launch", fake_launch)
    monkeypatch.setenv("SEMWEEVER_MODEL", "gpt-6-luna")
    monkeypatch.setenv("SEMWEEVER_WIRE_API", "responses")
    monkeypatch.setenv("SEMWEEVER_REASONING_EFFORT", "high")
    monkeypatch.setattr(sys, "argv", [
        "loop", "--semweaver-root", str(tmp_path), "--config", str(patch),
        "--initial-query", str(initial), "--patch", str(patch),
        "--source-root", str(tmp_path),
        "--vulnerable-db", str(vulnerable), "--fixed-db", str(fixed),
        "--codeql", str(codeql), "--output-dir", str(output),
        "--max-rounds", "1",
    ])

    assert loop.main() == (0 if expected_success else 2)
    result = json.loads((output / "RUN_RESULT.json").read_text())
    assert result["success"] is expected_success
    assert result["status"] == (
        "automatic_paired_pds" if expected_success
        else "partial_target_hit_recovered_with_fixed_noise"
    )
    assert result["total_model_calls"] == 1
    assert result["rows"][0]["pds"] is expected_success
    assert result["best_hit_recovery_with_fixed_noise"]["fixed_rows"] == fixed_after
    assert len(calls) == 3  # starting replay, model edit, candidate replay
