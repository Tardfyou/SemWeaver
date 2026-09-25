import unittest
from types import SimpleNamespace
from unittest.mock import patch

from langchain_core.messages import AIMessage

from src.refine.agent import LangChainRefinementAgent, ModelRateLimitExhausted


class _FakeRateLimit(Exception):
    status_code = 429


class _BoundModel:
    def __init__(self, failures: int, error: Exception | None = None):
        self.failures = failures
        self.error = error or _FakeRateLimit("gateway_concurrency_limit")
        self.calls = 0

    def invoke(self, _messages):
        self.calls += 1
        if self.calls <= self.failures:
            raise self.error
        return AIMessage(
            content='{"action":"finish"}',
            usage_metadata={"input_tokens": 3, "output_tokens": 2, "total_tokens": 5},
        )


class _BindingModel:
    model_name = "test-model"

    def __init__(self, bound):
        self.bound = bound
        self.bind_calls = []
        self.unbound_calls = 0

    def bind(self, **kwargs):
        self.bind_calls.append(kwargs)
        return self.bound

    def invoke(self, _messages):
        self.unbound_calls += 1
        raise AssertionError("unbound invocation must not occur")


def _agent(model, events, attempts=3):
    return LangChainRefinementAgent(
        config={
            "agent": {"require_json_mode": True},
            "llm": {
                "rate_limit": {
                    "max_attempts": attempts,
                    "initial_backoff_seconds": 0,
                    "max_backoff_seconds": 0,
                }
            },
        },
        llm_override=model,
        progress_callback=events.append,
    )


class RefineRateLimitTests(unittest.TestCase):
    @patch("src.refine.agent.time.sleep")
    def test_429_retries_same_json_bound_invoker(self, sleep):
        events = []
        bound = _BoundModel(failures=2)
        model = _BindingModel(bound)

        answer = _agent(model, events)._invoke_json_prompt("system", "prompt", phase="decide")

        self.assertEqual(answer, '{"action":"finish"}')
        self.assertEqual(bound.calls, 3)
        self.assertEqual(model.unbound_calls, 0)
        self.assertEqual(model.bind_calls, [{"response_format": {"type": "json_object"}}])
        self.assertEqual(sum(e.get("event") == "model_rate_limit_retry" for e in events), 2)
        self.assertEqual(sleep.call_count, 2)

    @patch("src.refine.agent.time.sleep")
    def test_exhaustion_is_typed_infrastructure_error(self, sleep):
        events = []
        bound = _BoundModel(failures=4)
        model = _BindingModel(bound)

        with self.assertRaises(ModelRateLimitExhausted) as caught:
            _agent(model, events)._invoke_json_prompt("system", "prompt", phase="decide")

        self.assertEqual(caught.exception.attempts, 3)
        self.assertEqual(bound.calls, 3)
        self.assertEqual(model.unbound_calls, 0)
        self.assertEqual(sum(e.get("event") == "model_rate_limit_exhausted" for e in events), 1)
        self.assertEqual(sleep.call_count, 2)

    def test_other_bound_errors_do_not_change_response_format(self):
        events = []
        bound = _BoundModel(failures=1, error=ValueError("bad request"))
        model = _BindingModel(bound)

        with self.assertRaisesRegex(ValueError, "bad request"):
            _agent(model, events)._invoke_json_prompt("system", "prompt", phase="decide")

        self.assertEqual(bound.calls, 1)
        self.assertEqual(model.unbound_calls, 0)

    def test_formal_treatment_requires_json_binding(self):
        events = []
        model = _BindingModel(bound=None)

        with self.assertRaisesRegex(RuntimeError, "JSON-object"):
            _agent(model, events)._invoke_json_prompt("system", "prompt", phase="decide")

        self.assertEqual(model.unbound_calls, 0)

    @patch("src.refine.agent.RefinementToolkit")
    @patch("src.refine.agent.RefinementTracker")
    def test_workflow_propagates_rate_limit_to_case_runner(self, _tracker, _toolkit):
        events = []
        agent = _agent(_BindingModel(_BoundModel(failures=0)), events)
        agent._evidence_provenance_gate = lambda _request: (True, "", {})
        agent._render_system_prompt = lambda: "system"
        agent._render_task_prompt = lambda _request: "task"
        workflow = SimpleNamespace(
            invoke=lambda *_args, **_kwargs: (_ for _ in ()).throw(
                ModelRateLimitExhausted(phase="decide", attempts=3)
            )
        )
        agent._build_workflow = lambda **_kwargs: workflow
        request = SimpleNamespace(
            work_dir="",
            max_iterations=2,
            patch_path="patch",
            target_path="checker",
            checker_name="checker",
        )

        with self.assertRaises(ModelRateLimitExhausted):
            agent.run(request)

        self.assertTrue(any(e.get("event") == "infrastructure_rate_limit_exhausted" for e in events))


if __name__ == "__main__":
    unittest.main()
