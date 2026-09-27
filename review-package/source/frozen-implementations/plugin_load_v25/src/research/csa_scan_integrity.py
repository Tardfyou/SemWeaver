"""Fail-closed scan-build diagnostics, independent of its build exit status."""
from __future__ import annotations

import hashlib
from pathlib import Path


def patch_ccc_mllvm_forwarding(source: str) -> str:
    """Preserve -mllvm and its operand in the analyzer's compile argument list."""
    anchor = "my %CompileOptionMap = (\n"
    if source.count(anchor) != 1:
        raise ValueError("Unsupported ccc-analyzer option-map layout")
    block = source.split(anchor, 1)[1].split(");", 1)[0]
    if "'-mllvm' => 1" in block:
        return source
    if "'-mllvm'" in block:
        raise ValueError("Unexpected existing -mllvm option arity")
    return source.replace(anchor, anchor + "  '-mllvm' => 1, # Preserve the LLVM option operand during analysis.\n", 1)


def scan_failure_records(root: Path) -> list[dict]:
    """A successful compiler process does not certify successful analysis."""
    root = Path(root)
    found = []
    if not root.exists():
        return found
    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue
        if path.name == "scan_error.log" or (
            "failures" in path.relative_to(root).parts
            and path.name.endswith((".info.txt", ".stderr.txt"))
        ):
            raw = path.read_bytes()
            if not raw.strip():
                continue
            text = raw.decode("utf-8", errors="replace")
            kind = "analyzer_option_error" if "Unknown command line argument" in text else "scan_build_failure_artifact"
            found.append({"path": str(path.relative_to(root)), "sha256": hashlib.sha256(raw).hexdigest(), "kind": kind, "diagnostic_tail": text[-1500:]})
    return found
