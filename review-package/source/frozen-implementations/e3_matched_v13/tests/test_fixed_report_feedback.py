import hashlib
from pathlib import Path

import pytest

from src.refine.fixed_report_feedback import summarize_fixed_reports


def test_frozen_fixed_report_summary_has_warning_path_and_hash(tmp_path: Path) -> None:
    first = tmp_path / "report-001.html"
    first.write_text(
        "<!-- BUGTYPE Double free -->\n"
        "<!-- BUGDESC Freed alias reused on retry -->\n"
        "<!-- BUGLINE 42 -->\n"
        '<div class="msg msgEvent">Freed pointer: <b>ptr</b></div>\n',
        encoding="utf-8",
    )
    (tmp_path / "report-002.html").write_text(
        "<!-- BUGDESC Another warning -->\n<div class='msg'>Second path</div>\n",
        encoding="utf-8",
    )

    text, bindings = summarize_fixed_reports(tmp_path, maximum=1)

    assert "Freed alias reused on retry" in text
    assert "Freed pointer: ptr" in text
    assert "Another warning" not in text
    assert bindings == [{
        "id": "report-001",
        "sha256": hashlib.sha256(first.read_bytes()).hexdigest(),
    }]


def test_fixed_report_summary_rejects_missing_warning(tmp_path: Path) -> None:
    (tmp_path / "report-001.html").write_text("<html><body>only source text</body></html>")
    with pytest.raises(ValueError, match="no warning"):
        summarize_fixed_reports(tmp_path)
