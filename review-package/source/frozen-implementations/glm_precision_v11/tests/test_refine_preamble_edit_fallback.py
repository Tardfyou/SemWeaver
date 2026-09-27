from src.refine.agent import LangChainRefinementAgent


def _agent(events):
    return LangChainRefinementAgent(
        config={}, llm_override=object(), progress_callback=events.append
    )


def test_unmatched_removal_only_preamble_does_not_block_exact_mechanism_edit():
    events = []
    agent = _agent(events)
    original = '#include "a.h"\n#include "b.h"\nint mechanism = 1;\n'
    result = agent._build_patch_from_exact_edits("checker.cpp", original, [
        {
            "old_snippet": '#include "a.h"\n#include "missing.h"',
            "new_snippet": '#include "a.h"',
        },
        {"old_snippet": "int mechanism = 1;", "new_snippet": "int mechanism = 2;"},
    ])

    assert result["error"] == ""
    assert result["skipped_nonsemantic_edits"] == [1]
    assert result["resulting_content"] == original.replace("mechanism = 1", "mechanism = 2")
    assert any(event.get("event") == "nonsemantic_preamble_edits_skipped" for event in events)


def test_unmatched_semantic_or_additive_edits_still_fail_closed():
    agent = _agent([])
    original = '#include "a.h"\nint mechanism = 1;\n'
    semantic = agent._build_patch_from_exact_edits("checker.cpp", original, [
        {"old_snippet": "int wrong = 1;", "new_snippet": "int mechanism = 2;"}
    ])
    additive = agent._build_patch_from_exact_edits("checker.cpp", original, [
        {"old_snippet": '#include "wrong.h"', "new_snippet": '#include "new.h"'}
    ])
    codeql_import = agent._build_patch_from_exact_edits("query.ql", "import cpp\n", [
        {"old_snippet": "import cpp\nimport missing", "new_snippet": "import cpp"}
    ])

    assert "未命中" in semantic["error"]
    assert "未命中" in additive["error"]
    assert "未命中" in codeql_import["error"]
