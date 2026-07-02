from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path


_CONTROL_KEYWORDS = {
    "if",
    "for",
    "while",
    "switch",
    "return",
    "sizeof",
}


@dataclass
class KernelFunction:
    file_path: Path
    code: str
    name: str
    start_line: int
    end_line: int

    def get_line_numbers(self) -> tuple[int, int]:
        return self.start_line, self.end_line

    @staticmethod
    def from_file(
        fpath: Path,
        node=None,
        rec_depth: int = 30,
        parser=None,
    ) -> list["KernelFunction"]:
        del node, rec_depth, parser
        text = Path(fpath).read_text(encoding="utf-8", errors="replace")
        return _extract_c_functions(Path(fpath), text)

    @staticmethod
    def from_files(files: list[Path], num_procs: int = 20) -> list["KernelFunction"]:
        del num_procs
        result: list[KernelFunction] = []
        for file_path in files:
            result.extend(KernelFunction.from_file(file_path))
        return result


def _extract_c_functions(file_path: Path, text: str) -> list[KernelFunction]:
    lines = text.splitlines()
    functions: list[KernelFunction] = []
    pending: list[str] = []
    pending_start = 1
    in_function = False
    brace_depth = 0
    function_start = 1
    function_name = ""
    body: list[str] = []

    for index, line in enumerate(lines, start=1):
        if in_function:
            body.append(line)
            brace_depth += line.count("{") - line.count("}")
            if brace_depth <= 0:
                functions.append(
                    KernelFunction(
                        file_path=file_path,
                        code="\n".join(body),
                        name=function_name,
                        start_line=function_start,
                        end_line=index,
                    )
                )
                in_function = False
                body = []
                brace_depth = 0
            continue

        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            pending = []
            pending_start = index + 1
            continue

        if not pending:
            pending_start = index
        pending.append(line)
        candidate = "\n".join(pending)
        name = _function_name(candidate)
        if name and "{" in line:
            in_function = True
            function_name = name
            function_start = pending_start
            body = pending[:]
            brace_depth = candidate.count("{") - candidate.count("}")
            pending = []
            if brace_depth <= 0:
                functions.append(
                    KernelFunction(
                        file_path=file_path,
                        code="\n".join(body),
                        name=function_name,
                        start_line=function_start,
                        end_line=index,
                    )
                )
                in_function = False
                body = []
                brace_depth = 0
            continue

        if stripped.endswith(";") or stripped.endswith("}") or len(pending) > 20:
            pending = []
            pending_start = index + 1

    return functions


def _function_name(signature: str) -> str:
    normalized = re.sub(r"/\*.*?\*/", " ", signature, flags=re.S)
    normalized = re.sub(r"\s+", " ", normalized).strip()
    if ";" in normalized or "(" not in normalized or ")" not in normalized or "{" not in normalized:
        return ""
    prefix = normalized.split("(", 1)[0].strip()
    if not prefix:
        return ""
    name = prefix.split()[-1].lstrip("*")
    name = name.split(".")[-1]
    if not re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", name):
        return ""
    if name in _CONTROL_KEYWORDS:
        return ""
    return name
