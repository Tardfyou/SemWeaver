"""Simple and clean LLM model interface supporting both cloud and local models"""

import os
import time
from email.utils import parsedate_to_datetime
from datetime import datetime, timezone
from typing import Any, Dict, Optional

from google import genai
from openai import APIConnectionError, APIStatusError, APITimeoutError, OpenAI

from global_config import global_config, logger

try:
    import anthropic

    ANTHROPIC_AVAILABLE = True
except ImportError:
    anthropic = None
    ANTHROPIC_AVAILABLE = False

# Global clients
clients: Dict[str, Any] = {}
client_protocols: Dict[str, str] = {}
usage_log = []


class ModelCallBudgetExhausted(RuntimeError):
    """The frozen semantic request cap was reached, excluding transport retries."""

model_config = {
    "model": "gpt-4o",
    "temperature": 1.0,
    "max_tokens": 16000,
}


def normalize_custom_base_url(value: str) -> str:
    """Keep explicitly versioned provider roots such as BigModel /v4."""
    base = value.rstrip("/")
    if base.endswith(("/v1", "/v4")):
        return base
    return base + "/v1"


def completion_token_parameter(model_name: str) -> str:
    """GLM's OpenAI-compatible Chat API uses max_tokens, not the newer alias."""
    return "max_tokens" if model_name.startswith(("glm-", "deepseek-")) else "max_completion_tokens"


def status_retry_delay(error: APIStatusError, attempt: int) -> float:
    """Honor provider Retry-After and back off long enough for 429 recovery."""
    if error.status_code != 429:
        return float(min(80, 5 * (2 ** attempt)))
    delay = float(min(120, 15 * (2 ** attempt)))
    header = getattr(getattr(error, "response", None), "headers", {}).get("retry-after", "")
    try:
        delay = max(delay, float(header))
    except (TypeError, ValueError):
        try:
            retry_at = parsedate_to_datetime(header)
            if retry_at.tzinfo is None:
                retry_at = retry_at.replace(tzinfo=timezone.utc)
            delay = max(delay, (retry_at - datetime.now(timezone.utc)).total_seconds())
        except (TypeError, ValueError, OverflowError):
            pass
    return float(min(300, max(0, delay)))


def init_llm():
    """Initialize LLM clients from configuration"""
    keys = global_config.get_key_config()

    # Environment fallbacks keep credentials out of replication-package files.
    if os.environ.get("OPENAI_API_KEY") and "openai_key" not in keys:
        keys["openai_key"] = os.environ["OPENAI_API_KEY"]
    if os.environ.get("ANTHROPIC_API_KEY") and "claude_key" not in keys:
        keys["claude_key"] = os.environ["ANTHROPIC_API_KEY"]
    if os.environ.get("DEEPSEEK_API_KEY") and "deepseek_key" not in keys:
        keys["deepseek_key"] = os.environ["DEEPSEEK_API_KEY"]
    custom_key = os.environ.get("CUSTOM_OPENAI_API_KEY")
    custom_base_url = os.environ.get("CUSTOM_OPENAI_BASE_URL")
    custom_wire_api = os.environ.get("CUSTOM_OPENAI_WIRE_API", "responses").lower()
    if custom_key and custom_base_url:
        normalized_base_url = normalize_custom_base_url(custom_base_url)
        clients["custom"] = OpenAI(
            api_key=custom_key,
            base_url=normalized_base_url,
            timeout=float(os.environ.get("CUSTOM_OPENAI_TIMEOUT", "900")),
            max_retries=0,
        )
        client_protocols["custom"] = custom_wire_api

    # Initialize OpenAI client (or compatible)
    if "openai_key" in keys:
        clients["openai"] = OpenAI(api_key=keys["openai_key"])
        client_protocols["openai"] = "chat_completions"

    # Initialize local/custom OpenAI-compatible client
    if "base_url" in keys:
        clients["local"] = OpenAI(
            base_url=keys["base_url"], api_key=keys.get("api_key", "dummy")
        )
        client_protocols["local"] = "chat_completions"

    # Initialize Claude client
    if "claude_key" in keys and ANTHROPIC_AVAILABLE:
        clients["claude"] = anthropic.Anthropic(api_key=keys["claude_key"])

    # Initialize Google client
    if "google_key" in keys:
        clients["google"] = genai.Client(api_key=keys["google_key"])

    # Initialize DeepSeek client
    if "deepseek_key" in keys:
        clients["deepseek"] = OpenAI(
            api_key=keys["deepseek_key"], base_url="https://api.deepseek.com/v1"
        )
        client_protocols["deepseek"] = "chat_completions"

    # Initialize custom providers
    providers = keys.get("providers", {})
    for name, config in providers.items():
        clients[name] = OpenAI(
            base_url=config["base_url"], api_key=config.get("api_key", "dummy")
        )
        client_protocols[name] = config.get("wire_api", "chat_completions")

    # Set model configuration from config.yaml
    model_config["model"] = global_config.get("model", "gpt-4o")
    model_config["temperature"] = global_config.get("temperature", 1.0)
    model_config["max_tokens"] = global_config.get("max_tokens", 16000)

    logger.info(f"Init LLM with model: {model_config['model']}")

    if not clients:
        raise ValueError("No LLM clients configured")


def get_client_and_model(model_name: str) -> tuple:
    """Determine which client to use and actual model name"""

    # Model to client mapping
    model_mapping = {
        # OpenAI models
        "gpt-4o": ("openai", "gpt-4o"),
        "o1": ("openai", "o1"),
        "o3-mini": ("openai", "o3-mini"),
        "o4-mini": ("openai", "o4-mini"),
        "o1-preview": ("openai", "o1-preview"),
        "gpt-5": ("openai", "gpt-5"),
        # Claude models
        "claude": ("claude", "claude-3-5-sonnet-20241022"),
        "claude-3-5-sonnet": ("claude", "claude-3-5-sonnet-20241022"),
        "claude-3-5-haiku": ("claude", "claude-3-5-haiku-20241022"),
        "claude-3-opus": ("claude", "claude-3-opus-20240229"),
        # Google models
        "google": ("google", "gemini-2.0-flash-exp"),
        "gemini": ("google", "gemini-2.0-flash-exp"),
        # DeepSeek models
        "deepseek-reasoner": ("deepseek", "deepseek-reasoner"),
        "deepseek-chat": ("deepseek", "deepseek-chat"),
    }

    # The matched-baseline runner uses an explicitly configured custom endpoint.
    # Prefer it for all unqualified model names so no request can accidentally
    # fall through to a developer's ambient OpenAI account.
    if "custom" in clients and ":" not in model_name:
        return clients["custom"], model_name

    # Check if it's a known model
    if model_name in model_mapping:
        client_name, actual_model = model_mapping[model_name]
        if client_name in clients:
            return clients[client_name], actual_model

    # Check if it's a local model (format: local:model_name)
    if model_name.startswith("local:") and "local" in clients:
        actual_model = model_name[6:]  # Remove "local:" prefix
        return clients["local"], actual_model

    # Check custom providers (format: provider:model_name)
    if ":" in model_name:
        provider, actual_model = model_name.split(":", 1)
        if provider in clients:
            return clients[provider], actual_model

    # Default to local client if available
    if "local" in clients:
        return clients["local"], model_name

    # Fallback to OpenAI if available
    if "openai" in clients:
        return clients["openai"], model_name

    raise ValueError(f"No client available for model {model_name}")


def _client_protocol(client: Any) -> str:
    for name, configured_client in clients.items():
        if configured_client is client:
            return client_protocols.get(name, "chat_completions")
    return "chat_completions"


def invoke_llm(
    prompt: str,
    temperature: Optional[float] = None,
    model: Optional[str] = None,
    max_tokens: Optional[int] = None,
) -> Optional[str]:
    """Invoke LLM with the given prompt"""

    model = model or model_config["model"]
    temperature = (
        temperature if temperature is not None else model_config["temperature"]
    )
    max_tokens = max_tokens or model_config["max_tokens"]

    configured_cap = int(os.environ.get("CUSTOM_OPENAI_MAX_MODEL_CALLS", "0") or 0)
    if configured_cap > 0 and len(usage_log) >= configured_cap:
        raise ModelCallBudgetExhausted(
            f"Frozen model-call cap reached: {len(usage_log)}/{configured_cap}"
        )

    logger.info(f"Start LLM process: {model}")

    # Simple token check
    if len(prompt) > 400000:  # ~100k tokens
        logger.warning("Prompt too long, skipping")
        return None

    # Get client and actual model name
    try:
        client, actual_model = get_client_and_model(model)
    except ValueError as e:
        logger.error(f"Error getting client and model for  {model}")
        logger.error(str(e))
        return None

    # Retry logic
    for attempt in range(6):
        try:
            # Handle different client types
            if isinstance(
                client, anthropic.Anthropic if ANTHROPIC_AVAILABLE else type(None)
            ):
                # Claude API
                response = client.messages.create(
                    model=actual_model,
                    messages=[{"role": "user", "content": prompt}],
                    max_tokens=max_tokens,
                    temperature=temperature,
                )
                answer = response.content[0].text

            elif isinstance(client, genai.Client):
                response = client.models.generate_content(
                    model=actual_model,
                    contents=prompt,
                )
                answer = response.text

            elif _client_protocol(client) == "responses":
                kwargs = {
                    "model": actual_model,
                    "input": prompt,
                    "max_output_tokens": max_tokens,
                }
                reasoning_effort = os.environ.get(
                    "CUSTOM_OPENAI_REASONING_EFFORT", "medium"
                ).strip()
                if reasoning_effort:
                    kwargs["reasoning"] = {"effort": reasoning_effort}
                response = client.responses.create(**kwargs)
                answer = response.output_text

            else:  # OpenAI Chat Completions or compatible
                kwargs = {
                    "model": actual_model,
                    "messages": [{"role": "user", "content": prompt}],
                }
                kwargs[completion_token_parameter(actual_model)] = max_tokens

                # Only add temperature for models that support it
                no_temp_models = ["o1", "o3-mini", "o4-mini", "o1-preview", "gpt-5"]
                if not any(m in actual_model for m in no_temp_models):
                    kwargs["temperature"] = temperature

                response = client.chat.completions.create(**kwargs)
                answer = response.choices[0].message.content

            usage = getattr(response, "usage", None)
            prompt_tokens = getattr(usage, "prompt_tokens", None)
            completion_tokens = getattr(usage, "completion_tokens", None)
            if prompt_tokens is None:
                prompt_tokens = getattr(usage, "input_tokens", 0)
            if completion_tokens is None:
                completion_tokens = getattr(usage, "output_tokens", 0)
            usage_log.append(
                {
                    "model": actual_model,
                    "prompt_tokens": int(prompt_tokens or 0),
                    "completion_tokens": int(completion_tokens or 0),
                    "total_tokens": int(getattr(usage, "total_tokens", 0) or 0),
                }
            )

            logger.info("Finish LLM process")

            # Remove think tags if present
            if answer and "<think>" in answer:
                answer = answer.split("</think>")[-1].strip()

            return answer

        except APIStatusError as e:
            # Authentication, permission, model-name, and request-shape failures
            # are deterministic for an unchanged request. Do not multiply calls
            # or cost by retrying them as transient transport failures.
            if e.status_code < 500 and e.status_code != 429:
                logger.error(f"Non-retryable API status {e.status_code}: {e}")
                raise
            logger.error(f"Error attempt {attempt + 1}: {e}")
            if attempt >= 5:
                logger.error("Failed too many times")
                raise
            delay = status_retry_delay(e, attempt)
            logger.warning(f"Provider status {e.status_code}; retrying after {delay:.0f}s")
            time.sleep(delay)
        except APITimeoutError as e:
            # A long generation may still be running upstream after the
            # client timeout. Reissuing it six times multiplies cost without
            # telling us whether the first call completed.
            logger.error(f"Model request timed out after the configured deadline: {e}")
            raise
        except APIConnectionError as e:
            logger.error(f"Connection attempt {attempt + 1}: {e}")
            if attempt >= 1:
                raise
            time.sleep(15)
        except Exception as e:
            logger.error(f"Non-status model error: {e}")
            raise

    return None


def get_embeddings(text: str) -> list:
    """Get embeddings using OpenAI API"""
    if "openai" not in clients:
        raise ValueError("OpenAI client required for embeddings")

    response = clients["openai"].embeddings.create(
        input=text, model="text-embedding-ada-002"
    )
    return response.data[0].embedding


# Backwards compatibility
def num_tokens_from_string(string: str) -> int:
    """Simple token approximation"""
    return len(string) // 4
