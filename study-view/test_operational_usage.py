"""Synthetic usage accounting; no fixtures contribute to experiment results."""
import json
import tempfile
import unittest
from pathlib import Path
from collect_operational_usage import collect,tokens


def save(path,rows):
    path.parent.mkdir(parents=True,exist_ok=True)
    path.write_text(''.join(json.dumps(r)+'\n' for r in rows))


class UsageTests(unittest.TestCase):
    def test_raw_truncated_reply_preferred_to_normalized_copy(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);attempt=root/'attempt'
            save(attempt/'provider_wire.jsonl',[
                {'event':'request','body':{'model':'synthetic'}},
                {'event':'response','body':{'id':'fixture-id','stop_reason':'max_tokens',
                                           'usage':{'input_tokens':3,'output_tokens':10}}}])
            save(attempt/'llm_exchanges.jsonl',[{'model':'synthetic','usage':{
                'available':True,'prompt_tokens':3,'completion_tokens':10}}])
            result=collect([root])
            self.assertEqual(result['unique_recorded_replies'],1)
            self.assertEqual(result['models']['synthetic']['reported_output_tokens'],10)
            self.assertEqual(result['rows'][0]['stop_reason'],'max_tokens')

    def test_copied_prefix_exchange_is_counted_once(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);event={'timestamp':1,'model':'synthetic','phase':'decide',
                'prompt_sha256':'p','response_sha256':'r','usage':{
                'available':True,'prompt_tokens':4,'completion_tokens':5}}
            for name in ('original','repair'):save(root/name/'llm_exchanges.jsonl',[event])
            result=collect([root]);self.assertEqual(result['unique_recorded_replies'],1)
            self.assertEqual(result['models']['synthetic']['reported_input_tokens'],4)

    def test_missing_usage_is_unknown_not_zero(self):
        self.assertFalse(tokens({},True)['available'])
        self.assertIsNone(tokens({},True)['input_tokens'])

    def test_baseline_returned_usage_is_included(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            save(root/'knighter/fixture/exchanges.jsonl',[
                {'event':'started','requested_settings':{'model':'synthetic'},'prompt_sha256':'p'},
                {'event':'returned','timestamp':2,'response_sha256':'r','model_response_count':1,
                 'usage_delta':[{'prompt_tokens':7,'completion_tokens':8}]}])
            result=collect([root]);self.assertEqual(result['recorded_fresh_baseline_replies'],1)
            self.assertEqual(result['models']['synthetic']['reported_output_tokens'],8)


if __name__=='__main__':unittest.main()
