"""JSON-object binding on the Messages wire must not be silently ignored."""

from types import SimpleNamespace

import pytest
from langchain_core.messages import HumanMessage, SystemMessage

import src.llm.anthropic_chat_model as module


def _model() -> module.AnthropicMessagesChatModel:
    return module.AnthropicMessagesChatModel(
        model="glm-5.3-flash", auth_token="test-only", base_url="https://example.invalid",
        max_tokens=256,
    )


def test_bound_json_mode_forces_tool_and_returns_object_json(monkeypatch):
    observed = {}

    def create(**request):
        observed.update(request)
        return SimpleNamespace(
            content=[SimpleNamespace(
                type="tool_use", name="emit_json",
                input={"action": "finish", "summary": "done"},
            )],
            stop_reason="tool_use",
            usage=SimpleNamespace(input_tokens=11, output_tokens=9),
        )

    monkeypatch.setattr(
        module, "Anthropic",
        lambda **kwargs: SimpleNamespace(messages=SimpleNamespace(create=create)),
    )
    result = _model().bind(response_format={"type": "json_object"}).invoke([
        SystemMessage(content="Return a decision"), HumanMessage(content="finish"),
    ])
    assert result.content == '{"action": "finish", "summary": "done"}'
    assert result.response_metadata["json_wire_mode"] == "forced_tool"
    assert observed["tool_choice"] == {"type": "tool", "name": "emit_json"}
    assert observed["tools"][0]["input_schema"]["required"] == ["action", "summary"]
    assert result.usage_metadata["output_tokens"] == 9


def test_bound_json_mode_rejects_output_limit(monkeypatch):
    monkeypatch.setattr(
        module, "Anthropic",
        lambda **kwargs: SimpleNamespace(messages=SimpleNamespace(create=lambda **request:
            SimpleNamespace(content=[], stop_reason="max_tokens", usage=None))),
    )
    with pytest.raises(RuntimeError, match="anthropic_json_tool_output_limit"):
        _model().bind(response_format={"type": "json_object"}).invoke([
            HumanMessage(content="return an object")
        ])


def test_valid_json_text_fallback_is_explicitly_marked(monkeypatch):
    monkeypatch.setattr(
        module, "Anthropic",
        lambda **kwargs: SimpleNamespace(messages=SimpleNamespace(create=lambda **request:
            SimpleNamespace(
                content=[SimpleNamespace(type="text", text='{"action":"finish","summary":"done"}')],
                stop_reason="end_turn", usage=SimpleNamespace(input_tokens=4, output_tokens=8),
            ))),
    )
    result = _model().bind(response_format={"type": "json_object"}).invoke([
        HumanMessage(content="finish")
    ])
    assert result.content == '{"action": "finish", "summary": "done"}'
    assert result.response_metadata["json_wire_mode"] == "validated_text_fallback"


def test_prose_without_json_decision_is_still_an_infrastructure_error(monkeypatch):
    monkeypatch.setattr(
        module, "Anthropic",
        lambda **kwargs: SimpleNamespace(messages=SimpleNamespace(create=lambda **request:
            SimpleNamespace(
                content=[SimpleNamespace(type="text", text="I cannot make a decision")],
                stop_reason="end_turn", usage=None,
            ))),
    )
    with pytest.raises(RuntimeError, match="anthropic_json_tool_missing_or_invalid"):
        _model().bind(response_format={"type": "json_object"}).invoke([
            HumanMessage(content="finish")
        ])


def test_json_string_tool_input_is_accepted_and_marked(monkeypatch):
    monkeypatch.setattr(
        module, "Anthropic",
        lambda **kwargs: SimpleNamespace(messages=SimpleNamespace(create=lambda **request:
            SimpleNamespace(
                content=[SimpleNamespace(
                    type="tool_use", name="emit_json",
                    input='{"action":"finish","summary":"done"}',
                )],
                stop_reason="tool_use", usage=SimpleNamespace(input_tokens=4, output_tokens=8),
            ))),
    )
    result = _model().bind(response_format={"type": "json_object"}).invoke([
        HumanMessage(content="finish")
    ])
    assert result.content == '{"action": "finish", "summary": "done"}'
    assert result.response_metadata["json_wire_mode"] == "validated_tool_input_json_string"


def test_unexpected_single_tool_name_requires_valid_decision_and_is_marked(monkeypatch):
    monkeypatch.setattr(
        module, "Anthropic",
        lambda **kwargs: SimpleNamespace(messages=SimpleNamespace(create=lambda **request:
            SimpleNamespace(
                content=[SimpleNamespace(
                    type="tool_use", name="provider_alias",
                    input={"action": "finish", "summary": "done"},
                )],
                stop_reason="tool_use", usage=SimpleNamespace(input_tokens=4, output_tokens=8),
            ))),
    )
    result = _model().bind(response_format={"type": "json_object"}).invoke([
        HumanMessage(content="finish")
    ])
    assert result.response_metadata["json_wire_mode"] == "forced_tool_name_fallback"
