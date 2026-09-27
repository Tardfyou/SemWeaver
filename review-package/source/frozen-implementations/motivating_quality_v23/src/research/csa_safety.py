"""Cheap, deterministic checks for CSA checker code that cannot terminate."""

from __future__ import annotations

import re


def nonterminating_ancestor_walks(code: str) -> list[dict[str, object]]:
    """Catch an inclusive-parent helper reapplied to its while-loop cursor.

    KNighter's findSpecificTypeInParents<T> returns its input when that input
    is already T. Reassigning a while-loop cursor with the same call therefore
    cannot advance it, even though the C++ checker compiles successfully.
    """
    scrubbed = re.sub(r"/\*.*?\*/|//[^\n]*", "", code, flags=re.DOTALL)
    hazards: list[dict[str, object]] = []
    for match in re.finditer(r"\bwhile\s*\(\s*(\w+)\s*\)\s*\{", scrubbed):
        cursor = match.group(1)
        depth = 1
        end = match.end()
        while end < len(scrubbed) and depth:
            depth += (scrubbed[end] == "{") - (scrubbed[end] == "}")
            end += 1
        if depth:
            continue
        body = scrubbed[match.end():end - 1]
        self_call = re.search(
            rf"\b{re.escape(cursor)}\s*=\s*"
            rf"findSpecificTypeInParents\s*<\s*(\w+)\s*>\s*"
            rf"\(\s*{re.escape(cursor)}\s*,",
            body,
        )
        if self_call:
            hazards.append({
                "kind": "inclusive_parent_lookup_on_same_loop_cursor",
                "cursor": cursor,
                "lookup_type": self_call.group(1),
                "line": scrubbed.count("\n", 0, match.start()) + 1,
            })
    return hazards
