"""Close a received final empty GLM reply as budgeted model-output failure.

No new inference, candidate edits, fabricated response or failed zero scan.
The prior normally executed selection is retained; original partial cell stays.
"""
import hashlib
import json
import shutil
import sys
import time
from pathlib import Path
from build_final39_summary import bound
from run_profile_cell import stop_reason

ROOT=Path(__file__).resolve().parent


def read(path):return json.loads(path.read_text())
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def save(path,value):
    with path.open('x') as handle:json.dump(value,handle,indent=2)


if __name__=='__main__':
    parent=Path(sys.argv[1]).resolve();relative=parent.relative_to(ROOT)
    assert relative.parts[0]=='e4-focused12'
    outer=read(parent/'RUN_MANIFEST.json');assert outer['status']=='partial' and outer['inputs_unchanged']
    plan=read(parent/'RUN_PLAN.json');assert all(sha(Path(p))==h for p,h in plan['inputs'].items())
    cfg=read(parent/'CONFIG.json');progress=read(parent/'PROGRESS.json')
    assert cfg['profile']['provider']=='anthropic' and plan['model_response_cap']==2
    assert progress['state']['calls']==1 and len(progress['rows'])==1
    first=progress['rows'][0];assert first['paired_valid']
    assert sha(Path(first['attempt'])/'RUN_MANIFEST.json')==first['manifest_sha256']
    bound(progress['best'])
    failed=parent/'continuation-02';wire=failed/'provider_wire.jsonl'
    events=[json.loads(line) for line in wire.read_text().splitlines()]
    requests=[e['body'] for e in events if e['event']=='request']
    replies=[e['body'] for e in events if e['event']=='response']
    assert len(requests)==len(replies)==1
    request,reply=requests[0],replies[0]
    assert request['model']==cfg['profile']['model'] and request['max_tokens']==65536
    assert request['extra_body']=={'reasoning_effort':'high','output_config':{'effort':'high'}}
    assert reply['stop_reason']=='max_tokens' and reply['usage']['output_tokens']==65536
    tools=[b for b in reply['content'] if b['type']=='tool_use']
    assert len(tools)==1 and tools[0]['name']=='emit_json' and tools[0]['input']=={}
    assert not (failed/'RUN_MANIFEST.json').exists() and not (failed/'llm_exchanges.jsonl').exists()
    output=ROOT/'profile-repairs'/relative;output.mkdir(parents=True,exist_ok=False)
    attempt=output/'received-invalid-final-reply';attempt.mkdir()
    unchanged=failed/'SAGenTestChecker.cpp'
    assert sha(unchanged)==read(Path(first['attempt'])/'RUN_MANIFEST.json')['candidate_sha256']
    shutil.copyfile(unchanged,attempt/'SAGenTestChecker.cpp')
    shutil.copyfile(wire,attempt/'provider_wire.jsonl')
    failure={'model':cfg['profile']['model'],'reasoning_effort':'high','model_calls_used':1,
             'max_tokens_per_call':65536,'success':False,'candidate_sha256':sha(unchanged),
             'starting_checker_sha256':sha(unchanged),'candidate_changed':False,
             'failure_type':'received_empty_tool_input_at_output_ceiling',
             'error_message':'Received reply exhausted output ceiling with empty emit_json input; no executable edit received.',
             'candidate_origin':'unchanged preceding candidate, not a newly generated edit',
             'raw_reply_sha256':sha(wire),'raw_reply_id':reply.get('id'),'raw_usage':reply['usage'],
             'additional_model_calls':0,'scored_execution':False}
    save(attempt/'RUN_MANIFEST.json',failure)
    state={**progress['state'],'calls':2,'invalid_streak':progress['state']['invalid_streak']+1}
    assert stop_reason(state,2)=='model_call_ceiling'
    rows=[first,{'attempt':str(attempt),'manifest_sha256':sha(attempt/'RUN_MANIFEST.json'),
                 'calls':1,'paired_valid':False,'adopted_improvement':False,'lost_controlled_hits':[],
                 'vulnerable_alerts':None,'fixed_alerts':None,'state_after':state}]
    save(output/'CONFIG.json',cfg)
    inputs={**plan['inputs'],str(Path(__file__)):sha(Path(__file__)),str(parent/'PROGRESS.json'):sha(parent/'PROGRESS.json'),
            str(parent/'RUN_MANIFEST.json'):sha(parent/'RUN_MANIFEST.json'),str(wire):sha(wire),
            str(output/'CONFIG.json'):sha(output/'CONFIG.json')}
    receipt={**plan,'inputs':inputs,'parent':str(parent),'operational_repair':'Received final empty tool input charged as model failure; retain previous normal selection; no new inference.'}
    save(output/'RUN_PLAN.json',receipt)
    budget=read(parent/'BUDGET.json');assert budget['dispatched_requests']>=2
    save(output/'BUDGET.json',budget)
    result={'status':'completed','case_id':cfg['case_id'],'arm':cfg['arm'],'profile':cfg['profile'],
            'state':state,'stop_reason':'model_call_ceiling','selected':progress['best'],'rows':rows,
            'boundary':'Pipeline budget completion, not successful second checker synthesis. Actual empty received tool input remains a failed model output, not a failed scan assigned0. Existing normal pair retained. All original records preserved.'}
    save(output/'RESULT.json',result)
    assert all(sha(Path(p))==h for p,h in inputs.items())
    save(output/'RUN_MANIFEST.json',{**receipt,'status':'completed','return_code':None,
        'inputs_unchanged':True,'finished_at':time.time(),'result_sha256':sha(output/'RESULT.json'),
        'exit_code_boundary':'Original interruption retained; this receipt certifies audited budget closure, not a new container execution.'})
    print(json.dumps({'case':cfg['case_id'],'received_responses_charged':2,'new_model_calls':0,
                      'selected_alerts':[result['selected']['vulnerable_alerts'],result['selected']['fixed_alerts']]}))
