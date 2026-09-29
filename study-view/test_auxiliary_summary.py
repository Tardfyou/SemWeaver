"""Synthetic closure/wire faults; no model calls or experiment outcomes."""
import json
import tempfile
import unittest
from pathlib import Path
from build_auxiliary_summary import require_complete,verify_glm_wire


class AuxiliaryTests(unittest.TestCase):
    def test_partial_matrix_refused(self):
        with self.assertRaises(AssertionError):require_complete({
            'e3':{'planned_new_cells':48,'verified_complete':47},
            'e4':{'planned_cells':36,'verified_complete':36}})

    def test_default_effort_cannot_be_mapped_high(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'wire.jsonl'
            rows=[{'event':'request','body':{'model':'glm-5.3','max_tokens':65536,
                    'extra_body':{'reasoning_effort':'high'}}},{'event':'response'}]
            path.write_text(''.join(json.dumps(r)+'\n' for r in rows))
            with self.assertRaises(AssertionError):verify_glm_wire(path,'glm-5.3')
            rows[0]['body']['extra_body']['output_config']={'effort':'high'}
            path.write_text(''.join(json.dumps(r)+'\n' for r in rows))
            verify_glm_wire(path,'glm-5.3')

    def test_unreceived_reply_not_assumed_complete(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'wire.jsonl'
            path.write_text(json.dumps({'event':'request','body':{'model':'glm-5.3','max_tokens':65536,
                'extra_body':{'reasoning_effort':'high','output_config':{'effort':'high'}}}})+'\n')
            with self.assertRaises(AssertionError):verify_glm_wire(path,'glm-5.3')


if __name__=='__main__':unittest.main()
