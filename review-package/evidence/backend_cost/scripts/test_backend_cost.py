import sys

import pytest

from measure_backend_cost import measured
from audit_backend_integration import describe


def test_success_collects_resources(tmp_path):
    result = measured([sys.executable, "-c", "print('retained output')"], tmp_path, "success")
    assert result["execution_valid"] and result["max_rss_kib"] > 0
    assert result["wall_seconds"] > 0
    assert (tmp_path / "success.stdout.log").read_text().strip() == "retained output"


def test_failure_is_not_success(tmp_path):
    result = measured([sys.executable, "-c", "raise SystemExit(7)"], tmp_path, "failure")
    assert not result["execution_valid"] and result["exit_code"] == 7


def test_timeout_is_not_a_negative_detection(tmp_path):
    result = measured([sys.executable, "-c", "import time; time.sleep(3)"], tmp_path, "timeout", timeout=0.05)
    assert not result["execution_valid"] and result["reason"] == "timeout"
    assert "rows" not in result


def test_footprint_is_explicit_physical_lines(tmp_path):
    source = tmp_path / "owned.py"
    source.write_text("# comment\n\ndef x():\n    return 1\n")
    result = describe(source, "owned.py", "test")
    assert result["physical_lines"] == 4
    assert result["nonblank_lines"] == 3
    assert result["functions"] == 1
    assert len(result["sha256"]) == 64
