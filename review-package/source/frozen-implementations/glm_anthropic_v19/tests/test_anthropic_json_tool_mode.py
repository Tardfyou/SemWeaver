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
