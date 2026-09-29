"""Actual retained empty-reply accounting; no inference or checker execution."""
import json
import unittest
from pathlib import Path
from build_final39_summary import bound

ROOT=Path(__file__).resolve().parent
REL=Path('e4-focused12/glm-5.3/repeat-1/G09_5aa2184e2908_Double_Free')


class ClosureTests(unittest.TestCase):
    def test_empty_final_reply_not_a_fake_zero_scan(self):
        path=ROOT/'profile-repairs'/REL
        data=json.loads((path/'RESULT.json').read_text())
        self.assertEqual(data['state']['calls'],2)
        self.assertEqual(sum(row['calls'] for row in data['rows']),2)
        last=data['rows'][-1]
        self.assertFalse(last['paired_valid'])
        self.assertIsNone(last['vulnerable_alerts'])
        self.assertIsNone(last['fixed_alerts'])
        manifest=json.loads((Path(last['attempt'])/'RUN_MANIFEST.json').read_text())
        self.assertFalse(manifest['success'])
        self.assertFalse(manifest['candidate_changed'])
        self.assertEqual(manifest['additional_model_calls'],0)
        bound(data['selected'])

    def test_original_failure_and_actual_reply_preserved(self):
        parent=ROOT/REL
        self.assertEqual(json.loads((parent/'RUN_MANIFEST.json').read_text())['status'],'partial')
        path=parent/'continuation-02/provider_wire.jsonl'
        replies=[json.loads(line)['body'] for line in path.read_text().splitlines()
                 if json.loads(line)['event']=='response']
        self.assertEqual(len(replies),1)
        self.assertEqual(replies[0]['stop_reason'],'max_tokens')
        self.assertEqual(replies[0]['usage']['output_tokens'],65536)
        blocks=[b for b in replies[0]['content'] if b['type']=='tool_use']
        self.assertEqual(blocks[0]['input'],{})


if __name__=='__main__':unittest.main()
