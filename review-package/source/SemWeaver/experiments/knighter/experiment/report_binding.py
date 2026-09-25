"""Deterministic binding from a scan-build HTML report to its build source."""

from __future__ import annotations

import posixpath
import re
from pathlib import Path


KERNEL_TOP_LEVEL = (
    "arch",
    "block",
    "certs",
    "crypto",
    "drivers",
    "fs",
    "include",
    "init",
    "ipc",
    "kernel",
    "lib",
    "mm",
    "net",
    "rust",
    "samples",
    "scripts",
    "security",
    "sound",
    "tools",
    "usr",
    "virt",
)


def normalize_kernel_path(source: str) -> str:
    normalized = source.strip().replace("\\", "/")
    marker = "/artifacts/external/linux/"
    if marker in normalized:
        normalized = normalized.split(marker, 1)[1]
    elif "/linux/" in normalized:
        normalized = normalized.rsplit("/linux/", 1)[1]
    elif normalized.startswith("/"):
        roots = "|".join(KERNEL_TOP_LEVEL)
        match = re.search(rf"/({roots})/.+\.c$", normalized)
        if match:
            normalized = match.group(0).lstrip("/")
    while normalized.startswith("./"):
        normalized = normalized[2:]
    normalized = posixpath.normpath(normalized)
    path = Path(normalized)
    if path.is_absolute() or normalized.startswith("../") or path.suffix != ".c":
        raise ValueError(f"Unsafe kernel build source: {source}")
    return path.as_posix()


def canonical_build_source_from_html(html: str) -> str:
    # BUGFILE can name a header where the diagnostic fired. The final `-x c`
    # argument identifies the translation unit and therefore the make object.
    invocations = re.findall(r"-x\s+c\s+([^\s<]+\.c)(?:\s|<)", html)
    if invocations:
        return normalize_kernel_path(invocations[-1])

    bugfile = re.search(r"<!--\s*BUGFILE\s+(.+?\.c)\s*-->", html)
    if bugfile:
        return normalize_kernel_path(bugfile.group(1))

    title = re.search(r"<title>\s*(.+?\.c)\s*</title>", html, re.IGNORECASE)
    if title:
        return normalize_kernel_path(title.group(1))
    raise ValueError("No C translation unit found in scan-build report")
