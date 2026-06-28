from __future__ import annotations

import os
from typing import Any, Dict, Optional

from langchain_core.language_models.chat_models import BaseChatModel
from langchain_openai import ChatOpenAI

from .anthropic_chat_model import AnthropicMessagesChatModel
from .provider_config import PROVIDER_CONFIGS, resolve_provider_name


def build_langchain_chat_model(
    config: Optional[Dict[str, Any]] = None,
    override: Any = None,
    *,
    temperature_key: Optional[str] = None,
    default_temperature: float = 0.2,
    generation_config_key: Optional[str] = None,
    temperature_override: Optional[float] = None,
) -> BaseChatModel:
    if isinstance(override, BaseChatModel):
        return override

    raw_config = config or {}
    llm_config = raw_config.get("llm", {}) if isinstance(raw_config.get("llm", {}), dict) else {}
    generation = llm_config.get("generation", {}) if isinstance(llm_config.get("generation", {}), dict) else {}
    api_keys = llm_config.get("api_keys", {}) if isinstance(llm_config.get("api_keys", {}), dict) else {}
    base_urls = llm_config.get("base_urls", {}) if isinstance(llm_config.get("base_urls", {}), dict) else {}
    agent_config = raw_config.get("agent", {}) if isinstance(raw_config.get("agent", {}), dict) else {}

    # 确定 provider
    provider = resolve_provider_name(llm_config.get("provider", "openai"))

    provider_info = PROVIDER_CONFIGS[provider]

    # 获取 API Key (优先配置文件，然后环境变量)
    api_key = str(api_keys.get(provider, "") or "").strip()
    if not api_key:
        env_key = provider_info.get("env_key", "")
        api_key = os.environ.get(env_key, "").strip()
    auth_token = ""
    auth_token_env_key = str(provider_info.get("auth_token_env_key", "") or "").strip()
    if auth_token_env_key:
        auth_token = str(api_keys.get(f"{provider}_auth_token", "") or "").strip()
        if not auth_token:
            auth_token = os.environ.get(auth_token_env_key, "").strip()

    if not api_key and not auth_token:
        raise ValueError(f"未配置 {provider.upper()} API Key，请在配置文件或环境变量中设置。")

    # 模型名称
    default_model = provider_info.get("default_model", "")
    model_name = str(llm_config.get("primary_model", default_model) or default_model).strip()

    # Base URL
    default_base_url = provider_info.get("base_url", "")
    base_url = str(base_urls.get(provider, default_base_url) or default_base_url).strip()
    if not base_url:
        base_url_env_key = str(provider_info.get("base_url_env_key", "") or "").strip()
        base_url = os.environ.get(base_url_env_key, "").strip() if base_url_env_key else ""

    # Temperature
    temperature = temperature_override
    if temperature is None:
        if temperature_key:
            temperature = agent_config.get(temperature_key, None)
        if temperature is None:
            temperature = agent_config.get("temperature", generation.get("temperature", default_temperature))

    scoped_generation = {}
    if generation_config_key:
        scoped_generation = llm_config.get(generation_config_key, {})
        if not isinstance(scoped_generation, dict):
            scoped_generation = {}

    max_tokens = scoped_generation.get("max_tokens", generation.get("max_tokens", 16384))

    temperature_value = float(temperature or default_temperature)
    timeout_value = float(generation.get("timeout", 120) or 120)
    max_retries_value = int(generation.get("max_retries", 3) or 3)
    max_tokens_value = int(max_tokens or 16384)

    if provider_info.get("wire_api") == "anthropic_messages":
        return AnthropicMessagesChatModel(
            model=model_name,
            api_key=api_key,
            auth_token=auth_token,
            base_url=base_url,
            max_tokens=max_tokens_value,
            timeout=timeout_value,
            max_retries=max_retries_value,
        )

    model_kwargs: Dict[str, Any] = {}
    if provider_info.get("wire_api") == "responses":
        model_kwargs["use_responses_api"] = True
        reasoning_effort = str(
            llm_config.get("reasoning_effort", provider_info.get("reasoning_effort", "")) or ""
        ).strip()
        if reasoning_effort:
            model_kwargs["reasoning_effort"] = reasoning_effort
        if "store" in provider_info or "store" in llm_config:
            model_kwargs["store"] = bool(llm_config.get("store", provider_info.get("store", True)))

    return ChatOpenAI(
        model=model_name,
        api_key=api_key,
        base_url=base_url,
        temperature=temperature_value,
        max_tokens=max_tokens_value,
        timeout=timeout_value,
        max_retries=max_retries_value,
        **model_kwargs,
    )
