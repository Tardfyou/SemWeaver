"""Preserve received Flash malformed first reply; use only the remaining reply."""
import fcntl
import hashlib
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path
from build_final39_summary import bound

ROOT=Path(__file__).resolve().parent;PROJECT=ROOT.parent


def read(path):return json.loads(path.read_text())
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def save(path,data):
    with path.open('x') as handle:json.dump(data,handle,indent=2)


if __name__=='__main__':
    parent=Path(sys.argv[1]).resolve();relative=parent.relative_to(ROOT)
    assert relative.parts[0]=='e4-focused12'
    outer=read(parent/'RUN_MANIFEST.json');assert outer['status']=='partial' and outer['inputs_unchanged']
    plan=read(parent/'RUN_PLAN.json');assert all(sha(Path(p))==h for p,h in plan['inputs'].items())
    cfg=read(parent/'CONFIG.json');initial=read(parent/'INITIAL_STATE.json')
    assert initial['state']['calls']==0 and not (parent/'PROGRESS.json').exists()
    assert cfg['profile']['provider']=='anthropic' and plan['model_response_cap']==2
    bound(initial['best'])
    original=parent/'continuation-01';wire=original/'provider_wire.jsonl'
    events=[json.loads(line) for line in wire.read_text().splitlines()]
    requests=[e['body'] for e in events if e['event']=='request'];replies=[e['body'] for e in events if e['event']=='response']
    assert len(requests)==len(replies)==1
    assert requests[0]['model']==cfg['profile']['model'] and requests[0]['max_tokens']==65536
    assert requests[0]['extra_body']=={'reasoning_effort':'high','output_config':{'effort':'high'}}
    tools=[b for b in replies[0]['content'] if b['type']=='tool_use']
    assert len(tools)==1 and tools[0]['name']=='emit_json' and isinstance(tools[0]['input'],dict)
    assert 'action' not in tools[0]['input'] and isinstance(tools[0]['input'].get('summary'),str)
    failed_manifest=original/'RUN_MANIFEST.json'
    failed_record=read(failed_manifest)
    assert not failed_record['success'] and failed_record['model_calls_used']==0
    assert 'anthropic_json_tool_missing_or_invalid' in failed_record['error_message']
    assert failed_record['candidate_sha256']==failed_record['starting_checker_sha256']==initial['best']['candidate_sha256']
    assert not (original/'llm_exchanges.jsonl').exists()
    output=ROOT/'profile-repairs'/relative
    lane=PROJECT/'LLM-Native/SemWeaver/artifacts/external/linux-state-recovery-v41'
    lock=(ROOT/'.profile-cap-spare-kernel.lock').open('a+');fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
    assert lane.is_dir();output.mkdir(parents=True,exist_ok=False)
    attempt=output/'received-invalid-prefix';attempt.mkdir()
    source=original/'SAGenTestChecker.cpp';assert sha(source)==initial['best']['candidate_sha256']
    shutil.copyfile(source,attempt/'SAGenTestChecker.cpp');shutil.copyfile(wire,attempt/'provider_wire.jsonl')
    manifest={'model':cfg['profile']['model'],'reasoning_effort':'high','model_calls_used':1,'success':False,
              'candidate_sha256':sha(source),'starting_checker_sha256':sha(source),'candidate_changed':False,
              'latest_failure_title':'received_model_output_contract_error',
              'latest_failure_text':'The received emit_json input omitted the required action field. No edit was applied. Return a complete decision object under the original contract.',
              'failure_type':'missing_required_action_in_received_tool_input','raw_reply_id':replies[0].get('id'),
              'raw_reply_sha256':sha(wire),'raw_usage':replies[0]['usage'],'new_inference':False}
    save(attempt/'RUN_MANIFEST.json',manifest);(attempt/'run_events.jsonl').open('x').close()
    state={**initial['state'],'calls':1,'invalid_streak':1}
    rows=[{'attempt':str(attempt),'manifest_sha256':sha(attempt/'RUN_MANIFEST.json'),'calls':1,
           'paired_valid':False,'adopted_improvement':False,'lost_controlled_hits':[],
           'vulnerable_alerts':None,'fixed_alerts':None,'state_after':state}]
    save(output/'PREFIX_PROGRESS.json',{'state':state,'best':initial['best'],'rows':rows})
    cfg.update(linux_dir=str(lane),prefix_parent=str(output),prefix_calls=1,prefix_attempts=[str(attempt)])
    save(output/'CONFIG.json',cfg)
    paths=[Path(__file__),ROOT/'run_mapped_high_received_prefix.py',ROOT/'run_profile_cap_repair.py',
           ROOT/'run_glm_mapped_high_cell.py',output/'CONFIG.json',output/'PREFIX_PROGRESS.json',
           attempt/'RUN_MANIFEST.json',attempt/'SAGenTestChecker.cpp',wire,failed_manifest,parent/'INITIAL_STATE.json',parent/'RUN_MANIFEST.json']
    inputs={**plan['inputs'],**{str(p):sha(p) for p in paths}}
    receipt={**plan,'inputs':inputs,'parent':str(parent),'prefix_calls':1,
             'operational_repair':'Actual invalid-format reply charged; one remaining reply, same mapped high/profile/budgets; no manual edit.'}
    save(output/'RUN_PLAN.json',receipt)
    prior=read(parent/'BUDGET.json');assert prior['dispatched_requests']>=1
    save(output/'BUDGET.json',{'deadline_epoch':plan['deadline_epoch'],'max_requests':prior['max_requests']-prior['dispatched_requests'],
         'dispatched_requests':0,'dispatches':[],'prior_dispatches':prior['dispatched_requests']})
    command=['docker','run','--rm','--name','semweaver-received-format-prefix-'+cfg['case_id'][:3],
        '-v',f'{PROJECT}:{PROJECT}','-v',f'{PROJECT/"LLM-Native"}:/work',
        '-v','/anonymous/home/.glm-key:/run/private/model-key:ro','-w','/work/SemWeaver-v43',
        '-e','PYTHONPATH=/work/SemWeaver-v43','-e','PATH=/usr/lib/llvm-18/bin:/usr/bin:/bin',
        '-e','GIT_CONFIG_COUNT=2','-e','GIT_CONFIG_KEY_0=safe.directory','-e',f'GIT_CONFIG_VALUE_0={lane}',
        '-e','GIT_CONFIG_KEY_1=safe.directory','-e',f'GIT_CONFIG_VALUE_1={PROJECT/"LLM-Native/SemWeaver-v43"}',
        'sha256:2ddb870c297ffbb9b30b901f84da5542a8ae37eb8d6ce332eae1657fdd0014ec',
        '/work/SemWeaver/.venv-dev/bin/python',str(ROOT/'run_mapped_high_received_prefix.py'),str(output/'CONFIG.json')]
    with (output/'container.stdout').open('x') as stdout,(output/'container.stderr').open('x') as stderr:
        code=subprocess.run(command,stdout=stdout,stderr=stderr,timeout=max(1,plan['deadline_epoch']-time.time())).returncode
    result=output/'RESULT.json';unchanged=all(sha(Path(p))==h for p,h in inputs.items())
    save(output/'RUN_MANIFEST.json',{**receipt,'status':'completed' if code==0 and unchanged and result.exists() else 'partial',
        'return_code':code,'inputs_unchanged':unchanged,'result_sha256':sha(result) if result.exists() else None,'finished_at':time.time()})
    print(json.dumps({'case':cfg['case_id'],'return_code':code,'prior_reply_charged':1,'remaining_reply_ceiling':1}),flush=True)
