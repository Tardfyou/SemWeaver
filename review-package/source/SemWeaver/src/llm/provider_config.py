from __future__ import annotations

from typing import Any, Dict


PROVIDER_CONFIGS: Dict[str, Dict[str, Any]] = {
    "openai": {
        "base_url": "https://api.openai.com/v1",
        "env_key": "OPENAI_API_KEY",
        "default_model": "gpt-4.1",
    },
    "deepseek": {
        "base_url": "https://api.deepseek.com",
        "env_key": "DEEPSEEK_API_KEY",
        "default_model": "deepseek-chat",
    },
    "openai_compatible": {
        "base_url": "",
        "env_key": "OPENAI_API_KEY",
        "base_url_env_key": "OPENAI_BASE_URL",
        "default_model": "gpt-4.1",
    },
    "anthropic": {
        "base_url": "https://api.anthropic.com",
        "env_key": "ANTHROPIC_API_KEY",
        "auth_token_env_key": "ANTHROPIC_AUTH_TOKEN",
        "default_model": "claude-opus-4-8",
        "wire_api": "anthropic_messages",
    },
}


def resolve_provider_name(raw_provider: Any) -> str:
    provider = str(raw_provider or "openai").strip().lower()
    if provider not in PROVIDER_CONFIGS:
        return "openai"
    return provider
