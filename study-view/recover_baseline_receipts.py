"""Recover missing parent receipts only after exact baseline ledgers pass."""
import hashlib
import json
import time
from pathlib import Path

ROOT=Path(__file__).resolve().parent
def read(path):return json.loads(path.read_text())
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()

if __name__=='__main__':
    for cell in sorted((ROOT/'knighter').iterdir()):
        if not cell.is_dir() or (cell/'EXECUTION_MANIFEST.json').exists():continue
        health_path=cell/'exchanges.health.json'
        if not health_path.exists():continue
        health=read(health_path)
        assert health['status']=='completed' and not health['errors']
        plan=read(cell/'RUN_PLAN.json')
        assert all(sha(p)==h for p,h in plan['inputs'].items())
        log=cell/'exchanges.jsonl'
        assert sha(log)==health['exchange_log_sha256']
        events=[json.loads(line) for line in log.read_text().splitlines()]
        starts=[e for e in events if e['event']=='started'];returns=[e for e in events if e['event']=='returned']
        assert len(starts)==health['logical_invoke_attempts']
        assert not any(e['event']=='failed_exception' for e in events)
        calls=sum(e['model_response_count'] for e in returns)
        assert calls==health['consumed_model_responses']<=plan['model_response_cap']
        for e in starts:
            settings=e['requested_settings']
            # Upstream report triage explicitly requests .01; refinement uses 0.
            # Preserve this recorded policy instead of relabelling every entry 0.
            assert settings['model']=='gpt-6-luna' and settings['max_tokens']==16384
            assert settings['temperature'] in (0.0,0.01)
        for e in returns:
            assert hashlib.sha256(e['response'].encode()).hexdigest()==e['response_sha256']
        summaries=list((cell/'output').glob('*/MATCHED_BASELINE_RESULT.json'))
        assert len(summaries)==1
        summary=read(summaries[0]);assert summary['execution_valid'] and summary['model_calls_used']==calls
        assert all(summary[k]==v for k,v in {'completed':True,'model':'gpt-6-luna',
                   'reasoning_effort':'high','temperature':0.0,'max_tokens':16384,'max_model_calls':32}.items())
        receipt={'case_id':plan['case_id'],'return_code':None,'status':'completed_baseline_pending_paired_replay',
                 'inputs_unchanged':True,'receipt_recovered_at':time.time(),'model_responses':calls,
                 'health_sha256':sha(health_path),'baseline_summary_sha256':sha(summaries[0]),
                 'recovery_script_sha256':sha(Path(__file__)),
                 'receipt_recovery':'Missing parent receipt; completed wrapper health, all input hashes, returned-response ledger and baseline summary verified. Parent exit-code observation unavailable, not invented.'}
        with (cell/'EXECUTION_MANIFEST.json').open('x') as handle:json.dump(receipt,handle,indent=2)
        print(json.dumps({'baseline_receipt_recovered':cell.name,'responses':calls,'new_model_calls':0}))
