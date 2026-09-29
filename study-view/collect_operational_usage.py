"""Provider-reported usage, including interrupted and superseded phase records.

This is an operational ledger, never an effectiveness table or billing estimate.
Raw Anthropic replies take precedence over their normalized exchange copies.
"""
from collections import Counter,defaultdict
import hashlib
import json
from pathlib import Path
import watch_final

ROOT=Path(__file__).resolve().parent


def events(path,issues):
    with path.open() as stream:
        for index,line in enumerate(stream,1):
            if not line.strip():continue
            try:yield index,json.loads(line)
            except ValueError:issues.append({'path':str(path),'line':index,'issue':'incomplete_or_invalid_json_snapshot'})


def tokens(usage,raw):
    keys=('input_tokens','output_tokens') if raw else ('prompt_tokens','completion_tokens')
    values=[usage.get(k) for k in keys]
    valid=all(isinstance(v,int) and not isinstance(v,bool) and v>=0 for v in values)
    return {'available':valid and (raw or usage.get('available') is True),
            'input_tokens':values[0] if valid else None,'output_tokens':values[1] if valid else None,
            'cache_read_input_tokens':usage.get('cache_read_input_tokens'),
            'cache_creation_input_tokens':usage.get('cache_creation_input_tokens')}


def collect(roots):
    wires=set();exchanges=set();baseline_logs=set();issues=[]
    for root in roots:
        wires.update(p.resolve() for p in root.rglob('provider_wire.jsonl'))
        exchanges.update(p.resolve() for p in root.rglob('llm_exchanges.jsonl'))
        baseline_logs.update(p.resolve() for p in (root/'knighter').glob('*/exchanges.jsonl'))
    rows=[];seen={};wire_attempts=set();requests=0;errors=Counter()
    def add(identity,row):
        if identity in seen:
            previous=seen[identity]
            if previous['reported_usage']!=row['reported_usage']:issues.append({'issue':'duplicate_usage_conflict','identity':identity})
            previous.setdefault('duplicate_record_paths',[]).append(row['record_path'])
        else:seen[identity]=row;rows.append(row)
    for path in sorted(wires):
        model=None
        for index,event in events(path,issues):
            if event.get('event')=='request':
                requests+=1;model=event.get('body',{}).get('model')
            elif event.get('event')=='transport_error':errors[str(event.get('status_code'))]+=1
            elif event.get('event')=='response':
                body=event['body'];wire_attempts.add(path.parent)
                response_id=body.get('id')
                identity='wire:'+str(response_id) if response_id else 'wire-location:'+str(path)+':'+str(index)
                add(identity,{'record_path':str(path),'record_line':index,'record_kind':'raw_provider_reply',
                    'requested_model':model,'returned_model':body.get('model'),'stop_reason':body.get('stop_reason'),
                    'timestamp':event.get('timestamp'),'reported_usage':tokens(body.get('usage',{}),True)})
    for path in sorted(exchanges):
        if path.parent in wire_attempts:continue
        for index,event in events(path,issues):
            identity='exchange:'+hashlib.sha256(json.dumps(
                [event.get(k) for k in ('timestamp','model','phase','prompt_sha256','response_sha256')],
                sort_keys=True).encode()).hexdigest()
            add(identity,{'record_path':str(path),'record_line':index,'record_kind':'normalized_exchange',
                'requested_model':event.get('model'),'returned_model':None,'stop_reason':None,
                'timestamp':event.get('timestamp'),'reported_usage':tokens(event.get('usage',{}),False)})
    baseline_reply_count=0
    for path in sorted(baseline_logs):
        model=None;prompt=None
        for index,event in events(path,issues):
            if event.get('event')=='started':
                model=event.get('requested_settings',{}).get('model');prompt=event.get('prompt_sha256')
            elif event.get('event')=='returned':
                count=event.get('model_response_count');usage=event.get('usage_delta',[])
                baseline_reply_count+=count if isinstance(count,int) else 0
                if count!=1 or len(usage)!=1:
                    issues.append({'path':str(path),'line':index,'issue':'baseline_reply_count_or_usage_shape'})
                    reported={'available':False,'input_tokens':None,'output_tokens':None}
                else:reported=tokens({**usage[0],'available':True},False)
                identity='baseline:'+hashlib.sha256(json.dumps(
                    [event.get('timestamp'),model,prompt,event.get('response_sha256')]).encode()).hexdigest()
                add(identity,{'record_path':str(path),'record_line':index,'record_kind':'baseline_recorded_reply',
                    'requested_model':model,'returned_model':None,'stop_reason':None,
                    'timestamp':event.get('timestamp'),'reported_usage':reported})
    totals=defaultdict(lambda:{'unique_recorded_replies':0,'known_usage_replies':0,
                             'unknown_usage_replies':0,'reported_input_tokens':0,'reported_output_tokens':0})
    for row in rows:
        block=totals[str(row['requested_model'])];usage=row['reported_usage']
        block['unique_recorded_replies']+=1
        if usage['available']:
            block['known_usage_replies']+=1;block['reported_input_tokens']+=usage['input_tokens'];block['reported_output_tokens']+=usage['output_tokens']
        else:block['unknown_usage_replies']+=1
    return {'status':'live_operational_snapshot','roots':[str(p) for p in roots],
            'unique_recorded_replies':len(rows),'recorded_wire_requests':requests,
            'recorded_fresh_baseline_replies':baseline_reply_count,
            'wire_transport_errors_by_status':dict(errors),'models':dict(totals),'audit_issues':issues,'rows':rows,
            'boundary':'Current-phase records plus exact reused main-cell/attempt roots; includes superseded configurations, truncated replies, negative/partial attempts and live requests. Fresh baseline logs included; two audited naturally stopped legacy baseline replies lack usage here and are separately reported as uncovered, never assumed free. Not a canonical comparison or billing estimate. Raw provider replies preferred to normalized duplicates; copied normalized records deduplicated by timestamp/model/phase/prompt/response identity. Missing usage unknown; token sums cover reported usage only. Requests without received replies are not presumed free or charged. Wire retry attempts hidden inside an SDK are not inferred.'}


if __name__=='__main__':
    snapshot=watch_final.snapshot();roots={ROOT}
    for pair in snapshot['rows']:
        for arm in ('native','no_internal'):
            cell=Path(pair[arm]['path'])
            if not cell.is_relative_to(ROOT):roots.add(cell)
            for name in ('RESULT.json','PROGRESS.json'):
                path=cell/name
                if path.exists():
                    for row in json.loads(path.read_text()).get('rows',[]):
                        attempt=Path(row['attempt'])
                        if not attempt.is_relative_to(ROOT):roots.add(attempt)
    output=collect(sorted(roots))
    closure=ROOT/'BASELINE39_CLOSURE.json'
    if closure.exists():
        baseline=json.loads(closure.read_text())
        output['verified_baseline_response_count']=sum(r['model_responses'] for r in baseline['rows'])
        output['audited_legacy_baseline_usage_uncovered']=[{'case_id':r['case_id'],'responses':r['model_responses'],
            'usage':'unknown_in_operational_ledger'} for r in baseline['rows'] if r.get('branch')=='audited_natural_stop']
    temporary=ROOT/'OPERATIONAL_USAGE_STATUS.tmp'
    temporary.write_text(json.dumps(output,indent=2));temporary.replace(ROOT/'OPERATIONAL_USAGE_STATUS.json')
    print(json.dumps({k:v for k,v in output.items() if k not in ('rows','boundary','roots')}))
