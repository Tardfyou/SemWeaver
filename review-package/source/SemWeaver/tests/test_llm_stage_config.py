import unittest
from unittest.mock import patch

from src.generate.agent import LangChainGenerateAgent
from src.llm.client import LLMClient
from src.llm.langchain_builder import build_langchain_chat_model
from src.refine.llm import build_langchain_chat_model as build_refine_chat_model
from src.tools.patch_analysis import PatchAnalysisTool
from src.utils.config import load_config


def _base_config():
    return {
        "llm": {
            "provider": "deepseek",
            "primary_model": "deepseek-chat",
            "api_keys": {"deepseek": "test-key"},
            "base_urls": {"deepseek": "https://api.deepseek.com"},
            "generation": {
                "temperature": 0.7,
                "max_tokens": 8192,
                "timeout": 30,
                "max_retries": 2,
            },
            "generate": {"max_tokens": 12000},
            "refine": {"max_tokens": 4000},
        },
        "agent": {
            "temperature": 0.3,
            "generate_temperature": 0.15,
            "generate_patch_analysis_temperature": 0.08,
            "generate_plan_temperature": 0.25,
            "generate_rag_check_temperature": 0.0,
            "generate_draft_temperature": 0.18,
            "generate_repair_temperature": 0.05,
            "refine_temperature": 0.1,
            "refine_decision_temperature": 0.08,
            "refine_repair_temperature": 0.04,
        },
    }


class _GenerateOnlyLLM:
    def __init__(self):
        self.calls = []

    def generate(self, prompt, temperature=None, max_tokens=None):
        self.calls.append(
            {
                "prompt": prompt,
                "temperature": temperature,
                "max_tokens": max_tokens,
            }
        )
        return "{}"


class LLMStageConfigTests(unittest.TestCase):
    @patch.dict(
        "os.environ",
        {
            "SEMWEEVER_PROVENANCE_GATE": "true",
            "SEMWEEVER_REFINE_MAX_ROUNDS": "1",
            "SEMWEEVER_WIRE_API": "responses",
            "SEMWEEVER_AGENT_MAX_ITERATIONS": "4",
            "SEMWEEVER_REFINE_MAX_TOKENS": "16000",
            "SEMWEEVER_INLINE_SEMANTIC_VALIDATION": "false",
            "SEMWEEVER_LLM_TIMEOUT": "900",
            "SEMWEEVER_LLM_MAX_RETRIES": "1",
        },
    )
    def test_fse_profile_environment_switches_are_typed(self):
        config = load_config("config/config.yaml")

        self.assertIs(config["quality_gates"]["evidence_provenance"]["enabled"], True)
        self.assertEqual(config["refine"]["max_rounds"], 1)
        self.assertEqual(config["llm"]["wire_api"], "responses")
        self.assertEqual(config["agent"]["max_iterations"], 4)
        self.assertEqual(config["llm"]["refine"]["max_tokens"], 16000)
        self.assertIs(config["refine"]["inline_semantic_validation"], False)
        self.assertEqual(config["llm"]["generation"]["timeout"], 900)
        self.assertEqual(config["llm"]["generation"]["max_retries"], 1)

    @patch("src.llm.langchain_builder.ChatOpenAI")
    def test_generate_builder_prefers_generate_max_tokens(self, chat_openai):
        config = _base_config()

        build_langchain_chat_model(
            config=config,
            generation_config_key="generate",
            temperature_override=0.25,
        )

        self.assertEqual(chat_openai.call_args.kwargs["max_tokens"], 12000)

    @patch("src.llm.langchain_builder.ChatOpenAI")
    def test_refine_builder_prefers_refine_max_tokens(self, chat_openai):
        config = _base_config()

        build_refine_chat_model(config=config)

        self.assertEqual(chat_openai.call_args.kwargs["max_tokens"], 4000)
        self.assertEqual(chat_openai.call_args.kwargs["temperature"], 0.08)

    @patch("src.llm.langchain_builder.ChatOpenAI")
    def test_refine_builder_allows_zero_sdk_retries(self, chat_openai):
        config = _base_config()
        config["llm"]["generation"]["max_retries"] = 0

        build_refine_chat_model(config=config)

        self.assertEqual(chat_openai.call_args.kwargs["max_retries"], 0)

    @patch("src.llm.langchain_builder.ChatOpenAI")
    def test_custom_responses_wire_omits_temperature(self, chat_openai):
        config = _base_config()
        config["llm"].update(
            {
                "provider": "openai_compatible",
                "wire_api": "responses",
                "primary_model": "gpt-5.6-terra",
                "reasoning_effort": "medium",
                "api_keys": {"openai_compatible": "test-key"},
                "base_urls": {"openai_compatible": "https://example.invalid/v1"},
            }
        )

        build_refine_chat_model(config=config)

        kwargs = chat_openai.call_args.kwargs
        self.assertTrue(kwargs["use_responses_api"])
        self.assertEqual(kwargs["reasoning_effort"], "medium")
        self.assertNotIn("temperature", kwargs)

    @patch("src.refine.agent.build_langchain_chat_model")
    def test_refine_agent_builds_phase_models_with_split_temperatures(self, build_model):
        build_model.side_effect = lambda **kwargs: object()

        from src.refine.agent import LangChainRefinementAgent

        LangChainRefinementAgent(config=_base_config())

        self.assertEqual(build_model.call_count, 2)
        overrides = [call.kwargs["temperature_override"] for call in build_model.call_args_list]
        self.assertEqual(overrides, [0.08, 0.04])

    def test_refine_generate_only_override_uses_phase_temperature(self):
        llm = _GenerateOnlyLLM()

        from src.refine.agent import LangChainRefinementAgent

        agent = LangChainRefinementAgent(config=_base_config(), llm_override=llm)

        agent._invoke_json_prompt("system", "prompt", phase="decide")
        agent._invoke_json_prompt("system", "prompt", phase="repair")

        self.assertEqual(llm.calls[0]["temperature"], 0.08)
        self.assertEqual(llm.calls[1]["temperature"], 0.04)

    @patch("src.llm.langchain_builder.build_langchain_chat_model")
    def test_generate_agent_builds_phase_models_with_split_temperatures(self, build_model):
        build_model.side_effect = lambda **kwargs: object()

        LangChainGenerateAgent(config=_base_config())

        self.assertEqual(build_model.call_count, 4)
        overrides = [call.kwargs["temperature_override"] for call in build_model.call_args_list]
        self.assertEqual(overrides, [0.25, 0.0, 0.18, 0.05])
        self.assertTrue(all(call.kwargs["generation_config_key"] == "generate" for call in build_model.call_args_list))

    def test_generate_only_override_uses_phase_temperature(self):
        llm = _GenerateOnlyLLM()
        agent = LangChainGenerateAgent(config=_base_config(), llm_override=llm)

        agent._invoke_json_prompt("system", "prompt", phase="plan")
        agent._invoke_json_prompt("system", "prompt", phase="repair")

        self.assertEqual(llm.calls[0]["temperature"], 0.25)
        self.assertEqual(llm.calls[1]["temperature"], 0.05)

    def test_patch_analysis_uses_split_temperature(self):
        tool = PatchAnalysisTool(prompt_config=_base_config())
        self.assertEqual(tool._analysis_temperature(), 0.08)

    @patch("src.llm.client.OpenAI")
    def test_llm_client_prefers_generate_max_tokens(self, _openai):
        client = LLMClient(_base_config()["llm"])
        self.assertEqual(client.max_tokens, 12000)

    @patch("src.llm.client.OpenAI")
    def test_llm_client_falls_back_to_shared_generation_max_tokens(self, _openai):
        config = _base_config()["llm"]
        config.pop("generate", None)
        client = LLMClient(config)
        self.assertEqual(client.max_tokens, 8192)

    @patch("src.llm.client.OpenAI")
    def test_llm_client_allows_responses_wire_override(self, _openai):
        config = _base_config()["llm"]
        config["wire_api"] = "responses"
        config["reasoning_effort"] = "medium"

        client = LLMClient(config)

        self.assertEqual(client.wire_api, "responses")
        self.assertEqual(client.reasoning_effort, "medium")
        self.assertFalse(client.responses_send_temperature)
        self.assertEqual(_openai.call_args.kwargs["max_retries"], 0)


if __name__ == "__main__":
    unittest.main()
