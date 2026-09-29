"""Synthetic evidence-ledger faults; no fixtures count as experiment samples."""
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
import collect_native_availability as ledger


def save(path,data):
    path.parent.mkdir(parents=True,exist_ok=True)
    path.write_text(json.dumps(data))


class AvailabilityTests(unittest.TestCase):
    def test_not_requested_needs_explicit_reason(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            save(root/'RUN_MANIFEST.json',{'checker_execution_probe_status':'not_requested'})
            row=ledger.audit_attempt(root)
            self.assertFalse(row['dynamic_evidence_available'])
            self.assertIn('unexplained_not_requested',row['integrity_errors'])

    def test_usable_label_does_not_override_tampered_raw_trace(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);probe=root/'checker_execution_probe';scans=[];sides=[]
            for side in ('vulnerable','fixed'):
                raw=probe/side/'traced/trace.jsonl';raw.parent.mkdir(parents=True)
                raw.write_text('synthetic-not-real-trace\n')
                digest=ledger.sha(raw)
                scans.extend([{'side':side,'mode':mode,'execution_valid':True,
                               'trace_capped':False,'trace_events':1 if mode=='traced' else 0,
                               'trace_sha256':digest if mode=='traced' else None}
                              for mode in ('original','traced')])
                sides.append({'side':side,'raw_sha256':digest})
            save(root/'RUN_MANIFEST.json',{'checker_execution_probe_status':'usable_hash_bound_trace',
                                          'starting_checker_sha256':'synthetic'})
            save(probe/'RESULT.json',{'execution_health':'completed','diagnostic_parity':True,
                                     'trace_usable':True,'scans':scans})
            save(probe/'RUN_MANIFEST.json',{'inputs_unchanged':True,'execution_health':'completed',
                                          'diagnostic_parity':True,'inputs':{},'scans':scans})
            save(root/'checker_execution_feedback.json',{'checker_sha256':'synthetic','sides':sides})
            self.assertTrue(ledger.audit_attempt(root)['dynamic_evidence_available'])
            (probe/'fixed/traced/trace.jsonl').write_text('tampered\n')
            row=ledger.audit_attempt(root)
            self.assertFalse(row['dynamic_evidence_available'])
            self.assertTrue(any(e.startswith('hash_mismatch:') for e in row['integrity_errors']))

    def test_prefix_attempts_are_not_double_counted(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);attempt=root/'attempt';cell=root/'cell'
            save(cell/'RESULT.json',{'rows':[{'attempt':str(attempt)},{'attempt':str(attempt)}]})
            snapshot={'verified_pairs':1,'rows':[{'complete_pair':True,'sample_id':'synthetic',
                                                 'native':{'path':str(cell)}}]}
            with patch.object(ledger,'audit_attempt',return_value={
                    'status':'synthetic','dynamic_evidence_available':False,'integrity_errors':[]}):
                result=ledger.collect(snapshot)
            self.assertEqual(result['unique_native_attempts'],1)
            self.assertEqual(len(result['rows']),1)

    def test_optional_cap_fallback_is_not_dynamic_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);attempt=root/'continuation-01-static'
            source=root/'starting.cpp';source.write_text('synthetic checker')
            checksum=ledger.sha(source);probe=root/'continuation-01/checker_execution_probe'
            scans=[{'execution_valid':True,'trace_capped':True} for _ in range(4)]
            result={'execution_health':'completed','diagnostic_parity':True,'trace_usable':False,'scans':scans}
            save(attempt/'RUN_MANIFEST.json',{'checker_execution_probe_status':'not_requested',
                                            'starting_checker_sha256':checksum})
            save(probe/'RESULT.json',result)
            save(probe/'RUN_MANIFEST.json',{'inputs_unchanged':True,'scans':scans,'inputs':{str(source):checksum}})
            save(root/'continuation-01-FALLBACK.json',{'reason':'unusable_optional_trace_coverage',
                 'model_calls_before_fallback':0,'not_dynamic_evidence':True,'probe':str(probe)})
            row=ledger.audit_attempt(attempt)
            self.assertFalse(row['dynamic_evidence_available'])
            self.assertEqual(row['status'],'unavailable_capped_optional_trace')
            self.assertEqual(row['integrity_errors'],[])


if __name__=='__main__':unittest.main()
