"""Keep analyzer timeout distinct from warning-volume termination."""

import subprocess
import sys
from pathlib import Path


BASELINE_SRC = (
    Path(__file__).resolve().parents[1]
    / "experiments" / "knighter" / "baseline" / "src"
)
sys.path.insert(0, str(BASELINE_SRC))
from tools import monitor_build_output  # noqa: E402


def test_scan_timeout_is_reported_as_timeout():
    process = subprocess.Popen(
        [sys.executable, "-c", "import time; time.sleep(5)"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    _, status = monitor_build_output(process, warning_limit=-1, timeout=0.2)
    assert status == "Timeout"


def test_warning_volume_is_not_reported_as_timeout():
    process = subprocess.Popen(
        [sys.executable, "-u", "-c",
         "import time; print('warning: a', flush=True); "
         "print('warning: b', flush=True); time.sleep(5)"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    _, status = monitor_build_output(process, warning_limit=1, timeout=5)
    assert status == "WarningLimit"
