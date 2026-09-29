"""Translation/provenance tests, not new model experiments."""
import json
from pathlib import Path
import tempfile
import unittest
from build_study_prompt_companions import attempt_prompts, request_view, language


class PromptCompanionTests(unittest.TestCase):
    def test_auxiliary_feedback_has_english_reading_form(self):
        source = '结构审查通过，但发现以下提示: 代码声明了 ProgramState，说明状态建模仍是空壳'
        value, missing = language.translated(source)
        self.assertFalse(missing)
        self.assertIn('Structural review passed', value)
        self.assertIn('remains a stub', value)
        self.assertEqual(source, '结构审查通过，但发现以下提示: 代码声明了 ProgramState，说明状态建模仍是空壳')

    def test_unknown_fragment_is_not_silently_removed(self):
        _, missing = language.translated('尚未登记的片段')
        self.assertTrue(missing)

    def test_actual_wire_preferred_over_replay_serialization(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path/'provider_wire.jsonl').write_text(json.dumps({'event': 'request', 'body':
                {'model': 'glm-5.3', 'system': 'actual system',
                 'messages': [{'role': 'user', 'content': 'actual original prompt'}]}})+'\n')
            (path/'llm_exchanges.jsonl').write_text('This deliberately is not an actual-request record.\n')
            rows = list(attempt_prompts(path))
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]['messages'][0]['content'], 'actual original prompt')
            self.assertEqual(rows[0]['kind'], 'actual_recorded_provider_request')

    def test_text_content_blocks_preserved(self):
        system, messages = request_view({'system': [{'type': 'text', 'text': 'system'}],
            'messages': [{'role': 'user', 'content': [{'type': 'text', 'text': 'first'}, {'type': 'text', 'text': 'second'}]}]})
        self.assertEqual(system, 'system')
        self.assertEqual(messages[0]['content'], 'first\nsecond')

    def test_nontext_content_not_discarded(self):
        with self.assertRaises(AssertionError):
            request_view({'messages': [{'role': 'user', 'content': [{'type': 'image'}]}]})


if __name__ == '__main__': unittest.main()
