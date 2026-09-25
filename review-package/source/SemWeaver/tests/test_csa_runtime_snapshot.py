import pytest

from src.evidence.collectors.artifact_extractor import (
    ProjectArtifactExtractor,
    SourceArtifactContext,
)
from src.evidence.collectors.csa_path import CSAPathEvidenceCollector
from src.core.analyzer_base import AnalyzerContext


def test_cfg_parser_keeps_function_scope_across_numbered_call_statements():
    dump = """
static void target(int ret)
 [B2]
  1: request_firmware
  2: [B2.1](&fw)
  3: ret
  4: [B2.3] = [B2.2]
  5: [B2.3] (ImplicitCastExpr, LValueToRValue, int)
  T: if [B2.5]
  Succs (2): B1 B0
Function: target calls: request_firmware release_firmware
"""
    context = SourceArtifactContext(
        patch_file="driver.c",
        resolved_file="/tmp/driver.c",
        relative_file="driver.c",
        hunk_index=0,
        anchor_line=10,
        function_name="target",
    )

    parsed = ProjectArtifactExtractor()._parse_csa_debug_dump(dump, context)

    assert parsed["branch_kinds"] == ["if"]
    assert parsed["branch_conditions"] == ["if [B2.5]"]
    assert parsed["resolved_branch_conditions"] == [
        "if ret (ImplicitCastExpr, LValueToRValue, int)"
    ]
    assert parsed["cfg_value_bindings"] == ["ret = request_firmware(&fw)"]
    assert parsed["call_edges"] == [
        "target -> request_firmware",
        "target -> release_firmware",
    ]


def test_patch_hunk_function_header_overrides_nearby_call_like_helper(tmp_path):
    source = """\
static int intended_target(int x)
{
    return with_runtime_pm(x);
}
"""
    source_path = tmp_path / "driver.c"
    source_path.write_text(source)
    patch_path = tmp_path / "change.patch"
    patch_path.write_text(
        """\
diff --git a/driver.c b/driver.c
--- a/driver.c
+++ b/driver.c
@@ -3,1 +3,1 @@ static int intended_target(int x)
-    return with_runtime_pm(x);
+    return (long)with_runtime_pm(x);
"""
    )
    extractor = ProjectArtifactExtractor()
    parsed = extractor.parse_patch(str(patch_path))
    hunk = parsed[0]["hunks"][0]

    assert hunk["section_header"] == "static int intended_target(int x)"
    assert extractor.hunk_function_hint(hunk) == "intended_target"
    context = extractor.find_named_function_context(
        source.splitlines(), 3, "intended_target"
    )
    assert context[0] == "intended_target"


def test_knighter_source_snapshot_uses_vulnerable_parent(tmp_path):
    patch_path = tmp_path / "change.patch"
    patch_path.write_text("commit 0123456789abcdef\n", encoding="utf-8")
    context = AnalyzerContext(
        patch_path=str(patch_path),
        output_dir=str(tmp_path),
        shared_analysis={"knighter": {"enabled": True}},
    )
    assert ProjectArtifactExtractor()._knighter_source_revision(context) == "0123456789abcdef^"


def test_knighter_source_snapshot_abstains_without_commit_id(tmp_path):
    patch_path = tmp_path / "change.patch"
    patch_path.write_text("diff --git a/driver.c b/driver.c\n", encoding="utf-8")
    context = AnalyzerContext(
        patch_path=str(patch_path),
        output_dir=str(tmp_path),
        shared_analysis={"knighter": {"enabled": True}},
    )
    with pytest.raises(ValueError, match="requires a commit ID"):
        ProjectArtifactExtractor()._knighter_source_revision(context)


def test_requested_revision_never_falls_back_to_mutable_worktree(tmp_path, monkeypatch):
    source = tmp_path / "driver.c"
    source.write_text("fixed-side source\n", encoding="utf-8")
    extractor = ProjectArtifactExtractor()
    monkeypatch.setattr(extractor, "_git_show_file", lambda *_args: None)
    lines, method = extractor._read_source_lines(
        project_root=tmp_path,
        resolved_file=source,
        patch_file={"old_path": "driver.c", "new_path": "driver.c"},
        source_revision="0123456789abcdef^",
    )
    assert lines == []
    assert method == "git_show_unavailable:0123456789abcdef^"


def test_collector_rejects_failed_runtime_snapshot():
    context = SourceArtifactContext(
        patch_file="driver.c",
        resolved_file="/tmp/driver.c",
        relative_file="driver.c",
        hunk_index=0,
        anchor_line=10,
        function_name="target",
    )
    runtime = {
        "cfg_snapshots": [
            {
                "source_file": "driver.c",
                "function_name": "target",
                "anchor_line": 10,
                "return_code": 1,
                "error": "missing generated header",
                "call_edges": ["target -> request_firmware"],
            }
        ]
    }

    assert CSAPathEvidenceCollector()._match_runtime_snapshot(context, runtime) is None


def test_collector_rejects_file_only_runtime_nearest_neighbor():
    context = SourceArtifactContext(
        patch_file="driver.c",
        resolved_file="/tmp/driver.c",
        relative_file="driver.c",
        hunk_index=0,
        anchor_line=50,
        function_name="",
    )
    runtime = {
        "cfg_snapshots": [
            {
                "source_file": "driver.c",
                "function_name": "unrelated_helper",
                "anchor_line": 51,
                "return_code": 0,
                "error": "",
                "call_edges": ["unrelated_helper -> sink"],
            }
        ]
    }

    assert CSAPathEvidenceCollector()._match_runtime_snapshot(context, runtime) is None


def test_runtime_reference_persists_interface_and_raw_hash():
    reference = CSAPathEvidenceCollector()._runtime_artifact_reference(
        {
            "interface": "clang-18:debug.DumpCFG+debug.DumpCallGraph",
            "output_schema": "semweaver.csa_cfg_snapshot.v1",
            "raw_output_path": "/artifact/cfg.txt",
            "raw_output_sha256": "abc",
            "command_sha256": "def",
            "unrelated": "ignored",
        }
    )

    assert reference == {
        "interface": "clang-18:debug.DumpCFG+debug.DumpCallGraph",
        "output_schema": "semweaver.csa_cfg_snapshot.v1",
        "raw_output_path": "/artifact/cfg.txt",
        "raw_output_sha256": "abc",
        "command_sha256": "def",
    }


def test_runtime_guard_record_retains_backend_relations_and_raw_binding():
    context = SourceArtifactContext(
        patch_file="driver.c",
        resolved_file="/tmp/driver.c",
        relative_file="driver.c",
        hunk_index=0,
        anchor_line=10,
        function_name="target",
        compile_command="clang -c driver.c",
    )
    runtime = {
        "cfg_snapshots": [
            {
                "source_file": "driver.c",
                "function_name": "target",
                "anchor_line": 10,
                "return_code": 0,
                "error": "",
                "branch_kinds": ["if"],
                "branch_conditions": ["if [B2.5]"],
                "resolved_branch_conditions": ["if ret"],
                "cfg_value_bindings": ["ret = request_firmware(&fw)"],
                "call_edges": ["target -> request_firmware"],
                "call_targets": ["request_firmware"],
                "interface": "clang-18:debug.DumpCFG+debug.DumpCallGraph",
                "output_schema": "semweaver.csa_cfg_snapshot.v1",
                "raw_output_path": "/artifact/cfg.txt",
                "raw_output_sha256": "abc",
                "command_sha256": "def",
            }
        ]
    }

    records = CSAPathEvidenceCollector()._runtime_guard_records(
        [context], runtime, "need path relation"
    )

    assert len(records) == 1
    assert records[0]["artifact"] == "clang-analyzer:debug-cfg"
    assert records[0]["payload"]["cfg_value_bindings"] == [
        "ret = request_firmware(&fw)"
    ]
    assert records[0]["payload"]["resolved_branch_conditions"] == ["if ret"]
    assert records[0]["payload"]["raw_output_sha256"] == "abc"
