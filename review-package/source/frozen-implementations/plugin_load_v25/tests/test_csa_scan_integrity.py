from pathlib import Path
import pytest
from src.research.csa_scan_integrity import patch_ccc_mllvm_forwarding, scan_failure_records


def test_option_operand_is_preserved_and_patch_is_idempotent():
    source = "my %CompileOptionMap = (\n  '-include' => 1,\n);\n"
    patched = patch_ccc_mllvm_forwarding(source)
    assert "'-mllvm' => 1" in patched
    assert patch_ccc_mllvm_forwarding(patched) == patched


def test_unknown_wrapper_layout_fails_closed():
    with pytest.raises(ValueError):
        patch_ccc_mllvm_forwarding("unrecognized wrapper")


def test_failure_artifact_invalidates_zero_report_scan(tmp_path):
    failure = tmp_path / "timestamp/failures/clang_error.stderr.txt"
    failure.parent.mkdir(parents=True)
    failure.write_text("Unknown command line argument '-mllvm'\n")
    rows = scan_failure_records(tmp_path)
    assert len(rows) == 1 and rows[0]["kind"] == "analyzer_option_error"
    assert len(rows[0]["sha256"]) == 64


def test_empty_stderr_with_failure_info_is_still_invalid(tmp_path):
    failure = tmp_path / "timestamp/failures/clang_error.info.txt"
    failure.parent.mkdir(parents=True)
    failure.write_text("target.c\nCrash\n")
    failure.with_name("clang_error.stderr.txt").touch()
    assert len(scan_failure_records(tmp_path)) == 1


def test_legitimate_empty_scan_is_not_failure(tmp_path):
    (tmp_path / "index.html").write_text("No bugs found")
    assert scan_failure_records(tmp_path) == []
