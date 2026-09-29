"""Record unique consumed attempts and observation timing; never invent billing."""
import json
from pathlib import Path
import watch_final

ROOT=Path(__file__).resolve().parent
def read(path):
    try:return json.loads(Path(path).read_text())
    except (OSError,json.JSONDecodeError):return {}

if __name__=='__main__':
    snapshot=watch_final.snapshot();rows=[]
    for pair in snapshot['rows']:
        if not pair['complete_pair']:continue
        for arm in ('native','no_internal'):
            cell=Path(pair[arm]['path']);result=read(cell/'RESULT.json')
            attempts={Path(r['attempt']) for r in result['rows']}
            usages=[];missing_usage=[]
            for attempt in sorted(attempts):
                path=attempt/'llm_exchanges.jsonl'
                if not path.exists():missing_usage.append(str(attempt));continue
                for line in path.read_text().splitlines():
                    event=json.loads(line);usage=event.get('usage',{})
                    if usage.get('available'):usages.append(usage)
                    else:missing_usage.append(str(attempt))
            probes=[];seen=set()
            for attempt in sorted(attempts):
                for p in attempt.glob('*probe/RESULT.json'):
                    real=p.resolve()
                    if real in seen:continue
                    seen.add(real);d=read(p)
                    probes.append({'path':str(p),'execution_health':d.get('execution_health'),
                                   'ast_seconds':d.get('ast_seconds'),'scans':[
                                      {k:s.get(k) for k in ('side','mode','seconds','execution_valid','trace_capped')}
                                      for s in d.get('scans',[])]})
            rows.append({'case_id':pair['sample_id'],'arm':arm,'cell':str(cell),
                         'actual_model_responses':result['state']['calls'],'unique_attempts':len(attempts),
                         'controlled_fixture_guard_enabled':arm=='native',
                         'guard_rejected_attempts':[{'attempt':r['attempt'],'lost_controlled_hits':r['lost_controlled_hits']}
                                                    for r in result['rows'] if r.get('lost_controlled_hits')],
                         'reported_prompt_tokens':sum(u.get('prompt_tokens',0) for u in usages),
                         'reported_completion_tokens':sum(u.get('completion_tokens',0) for u in usages),
                         'reported_total_tokens':sum(u.get('total_tokens',0) for u in usages),
                         'missing_usage_records':missing_usage,'recorded_observation_probes':probes})
    output={'status':'complete' if snapshot['verified_pairs']==39 else 'partial_completed_pairs_only',
            'completed_pairs':snapshot['verified_pairs'],'planned_pairs':39,'rows':rows,
            'boundary':'Unique scored-chain attempts, including charged prefixes, never counted twice across repair directories. Provider-reported usage is not billing. Controlled-fixture retention guard is native-only and separately disclosed; its rejection record is not target recall. Probe scan elapsed sums are not end-to-end/CPU time; missing timing is unknown. Interrupted parents, truncations, retries and independent validation/fixture costs require a separate operational ledger before full cost claims.'}
    temporary=ROOT/'MAIN_COST_STATUS.tmp';temporary.write_text(json.dumps(output,indent=2))
    temporary.replace(ROOT/'MAIN_COST_STATUS.json')
    print(json.dumps({'completed_pairs':output['completed_pairs'],'status':output['status'],
                      'missing_usage_records':sum(len(r['missing_usage_records']) for r in rows)}))
