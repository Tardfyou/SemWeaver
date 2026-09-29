import copy
import unittest
from replay_received_summary_entry import normalized_payload


class ReplayTests(unittest.TestCase):
    def reply(self):
        return {'stop_reason': 'tool_use', 'content': [{'type': 'tool_use', 'name': 'emit_json',
                'input': {'action': 'apply_patch', 'edits': [{'old_snippet': 'before', 'new_snippet': 'after'}]}}]}

    def test_only_metadata_added_original_unchanged(self):
        reply = self.reply()
        before = copy.deepcopy(reply)
        result = normalized_payload(reply)
        self.assertEqual(reply, before)
        self.assertEqual(result['summary'], '')
        self.assertEqual(result['edits'], before['content'][0]['input']['edits'])

    def test_missing_action_not_inferred(self):
        r = self.reply()
        del r['content'][0]['input']['action']
        with self.assertRaises(AssertionError): normalized_payload(r)

    def test_truncation_not_salvaged(self):
        r = self.reply()
        r['stop_reason'] = 'max_tokens'
        with self.assertRaises(AssertionError): normalized_payload(r)

    def test_redirected_edit_refused(self):
        r = self.reply()
        r['content'][0]['input']['edits'][0]['path'] = '/original/checker.cpp'
        with self.assertRaises(AssertionError): normalized_payload(r)


if __name__ == '__main__': unittest.main()
