from src.refine.agent import LangChainRefinementAgent


def test_complete_final_decision_survives_truncated_prefix():
    agent = LangChainRefinementAgent(config={})
    response = (
        '{"action":"apply_patch","edits":[{"old_snippet":"broken'
        '\n{"action":"apply_patch","summary":"complete",'
        '"edits":[{"old_snippet":"old","new_snippet":"new"}]}'
    )

    decision, error = agent._parse_decision(response)

    assert error == ""
    assert decision["action"] == "apply_patch"
    assert decision["summary"] == "complete"
    assert decision["edits"] == [{"old_snippet": "old", "new_snippet": "new"}]


def test_valid_outer_json_takes_precedence_over_embedded_action_text():
    agent = LangChainRefinementAgent(config={})
    response = (
        '{"action":"apply_patch","edits":[{"old_snippet":"old",'
        '"new_snippet":"\\n{\\\"action\\\":\\\"finish\\\"}"}]}'
    )

    decision, error = agent._parse_decision(response)

    assert error == ""
    assert decision["action"] == "apply_patch"
    assert len(decision["edits"]) == 1


def test_incomplete_second_object_is_not_spliced_or_accepted():
    agent = LangChainRefinementAgent(config={})
    response = (
        '{"action":"apply_patch","edits":[{"old_snippet":"broken'
        '\n{"action":"apply_patch","edits":[{"old_snippet":"old"}'
    )

    decision, error = agent._parse_decision(response)

    assert decision == {}
    assert "JSON" in error
