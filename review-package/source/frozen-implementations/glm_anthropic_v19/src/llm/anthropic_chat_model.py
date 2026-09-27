from __future__ import annotations

import json
from typing import Any, Dict, List, Optional

from anthropic import Anthropic
from langchain_core.callbacks.manager import CallbackManagerForLLMRun
from langchain_core.language_models.chat_models import BaseChatModel
from langchain_core.messages import AIMessage, BaseMessage, HumanMessage, SystemMessage
from langchain_core.outputs import ChatGeneration, ChatResult

from .usage import normalize_usage


class AnthropicMessagesChatModel(BaseChatModel):
    model: str
    api_key: str = ""
    auth_token: str = ""
    base_url: str
    max_tokens: int
    timeout: float = 120.0
    max_retries: int = 1

    @property
    def _llm_type(self) -> str:
        return "anthropic_messages"

    @property
    def model_name(self) -> str:
        return self.model

    def _generate(
        self,
        messages: List[BaseMessage],
        stop: Optional[List[str]] = None,
        run_manager: Optional[CallbackManagerForLLMRun] = None,
        **kwargs: Any,
    ) -> ChatResult:
        system_parts: List[str] = []
        anthropic_messages: List[Dict[str, Any]] = []
        for msg in messages:
            content = self._content_to_text(msg.content)
            if isinstance(msg, SystemMessage):
                system_parts.append(content)
            elif isinstance(msg, HumanMessage):
                anthropic_messages.append({"role": "user", "content": content})
            elif isinstance(msg, AIMessage):
                anthropic_messages.append({"role": "assistant", "content": content})
            else:
                role = "assistant" if getattr(msg, "type", "") == "ai" else "user"
                anthropic_messages.append({"role": role, "content": content})

        client_kwargs: Dict[str, Any] = {
            "base_url": self.base_url,
            "timeout": self.timeout,
            "max_retries": self.max_retries,
        }
        if self.auth_token:
            client_kwargs["auth_token"] = self.auth_token
        else:
            client_kwargs["api_key"] = self.api_key
        client = Anthropic(**client_kwargs)

        request: Dict[str, Any] = {
            "model": self.model,
            "max_tokens": int(self.max_tokens),
            "messages": anthropic_messages,
        }
        if system_parts:
            request["system"] = "\n\n".join(system_parts)
        if stop:
            request["stop_sequences"] = stop

        response_format = kwargs.get("response_format")
        json_object_mode = response_format == {"type": "json_object"}
        if response_format is not None and not json_object_mode:
            raise ValueError(f"Unsupported Anthropic response format: {response_format}")
        if json_object_mode:
            # The Messages API has no OpenAI response_format field. A forced
            # object-valued tool call gives an actual structured-output
            # contract instead of silently ignoring the requested JSON mode.
            request["tools"] = [{
                "name": "emit_json",
                "description": "Return the refinement decision as a JSON object without prose.",
                "input_schema": {
                    "type": "object",
                    "properties": {
                        "action": {"type": "string"},
                        "summary": {"type": "string"},
                        "path": {"type": "string"},
                        "recursive": {"type": "boolean"},
                        "edits": {"type": "array", "items": {"type": "object"}},
                        "evidence_types": {"type": "array", "items": {"type": "string"}},
                    },
                    "required": ["action", "summary"],
                    "additionalProperties": True,
                },
            }]
            request["tool_choice"] = {"type": "tool", "name": "emit_json"}

        response = client.messages.create(**request)
        blocks = getattr(response, "content", None) or []
        if json_object_mode:
            tool_blocks = [
                block for block in blocks
                if getattr(block, "type", "") == "tool_use"
                and getattr(block, "name", "") == "emit_json"
            ]
            if len(tool_blocks) != 1 or not isinstance(
                getattr(tool_blocks[0], "input", None), dict
            ):
                if getattr(response, "stop_reason", "") == "max_tokens":
                    raise RuntimeError("anthropic_json_tool_output_limit")
                raise RuntimeError("anthropic_json_tool_missing_or_invalid")
            text = json.dumps(tool_blocks[0].input, ensure_ascii=False)
        else:
            text = "".join(
                str(block.text)
                for block in blocks
                if getattr(block, "type", "") == "text" and getattr(block, "text", None)
            )
        usage = normalize_usage(getattr(response, "usage", None), model=self.model)
        message = AIMessage(
            content=text,
            response_metadata={
                "model_name": self.model,
                "usage": usage,
                "raw": response,
            },
            usage_metadata={
                "input_tokens": usage["prompt_tokens"],
                "output_tokens": usage["completion_tokens"],
                "total_tokens": usage["total_tokens"],
            }
            if usage["available"]
            else None,
        )
        return ChatResult(generations=[ChatGeneration(message=message)])

    @staticmethod
    def _content_to_text(content: Any) -> str:
        if isinstance(content, str):
            return content
        if isinstance(content, list):
            parts: List[str] = []
            for item in content:
                if isinstance(item, str):
                    parts.append(item)
                elif isinstance(item, dict):
                    value = item.get("text") or item.get("content")
                    if value:
                        parts.append(str(value))
                else:
                    parts.append(str(item))
            return "\n".join(parts)
        return str(content)
