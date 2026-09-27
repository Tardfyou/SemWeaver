"""Compact, deterministic CSA fixed-report feedback shared with the baseline."""

from __future__ import annotations

import hashlib
import re
from html.parser import HTMLParser
from pathlib import Path


class _MessageParser(HTMLParser):
    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.messages: list[str] = []
        self._depth = 0
        self._parts: list[str] = []

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        if self._depth and tag not in {"br", "hr", "img", "input", "meta", "link"}:
            self._depth += 1
        elif tag == "div" and "msg" in dict(attrs).get("class", "").split():
            self._depth = 1
            self._parts = []

    def handle_endtag(self, tag: str) -> None:
        if not self._depth:
            return
        self._depth -= 1
        if self._depth == 0:
            message = " ".join(" ".join(self._parts).split())
            if message and message not in self.messages:
                self.messages.append(message)

    def handle_data(self, data: str) -> None:
        if self._depth:
            self._parts.append(data)


def _metadata(html: str, name: str) -> str:
    match = re.search(rf"<!--\s*{re.escape(name)}\s+(.*?)\s*-->", html, re.DOTALL)
    return " ".join(match.group(1).split()) if match else ""


def summarize_fixed_reports(
    directory: Path, *, maximum: int = 5, recursive: bool = False,
) -> tuple[str, list[dict]]:
    """Return the same first five frozen report IDs offered to KNighter.

    This is analyzer *output*, not analyzer-internal evidence. No source oracle,
    vulnerability label, or manually edited report text is introduced here.
    """
    directory = directory.resolve()
    if not directory.is_dir():
        raise FileNotFoundError(directory)
    reports = sorted(
        directory.rglob("report-*.html") if recursive else directory.glob("report-*.html")
    )[:maximum]
    if not reports:
        raise ValueError(f"No fixed-side CSA HTML reports in {directory}")
    rendered: list[str] = []
    bindings: list[dict] = []
    for report in reports:
        raw = report.read_bytes()
        html = raw.decode("utf-8", errors="replace")
        parser = _MessageParser()
        parser.feed(html)
        meta = {name: _metadata(html, name) for name in (
            "BUGTYPE", "BUGDESC", "BUGFILE", "BUGLINE", "BUGCATEGORY",
        )}
        events = parser.messages[:12]
        if not meta["BUGDESC"] and not events:
            raise ValueError(f"CSA report has no warning or path messages: {report}")
        bindings.append({
            "id": report.stem,
            "sha256": hashlib.sha256(raw).hexdigest(),
            **({"relative_path": str(report.relative_to(directory))} if recursive else {}),
        })
        details = [
            f"fixed report {report.stem}:",
            *(f"{name}={value}" for name, value in meta.items() if value),
            *(f"path event {index}: {event[:350]}" for index, event in enumerate(events, 1)),
        ]
        rendered.append("\n".join(details)[:3500])
    return "\n\n".join(rendered), bindings
