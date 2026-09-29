"""Recover a verified capped profile probe without dropping consumed replies."""
import fcntl
import json
import subprocess
import sys
import time
from pathlib import Path
import launch_contextless_repair as base

ROOT=Path(__file__).resolve().parent
PROJECT=ROOT.parent

if __name__=='__main__':
    parent=Path(sys.argv[1]).resolve();relative=parent.relative_to(ROOT)
    assert relative.parts[0] in ('e3-selected-repeats','e4-common64')
    outer=base.read(parent/'RUN_MANIFEST.json');assert outer['status']=='partial' and outer['inputs_unchanged']
    plan=base.read(parent/'RUN_PLAN.json');assert all(base.sha(p)==h for p,h in plan['inputs'].items())
    cfg=base.read(parent/'CONFIG.json');assert cfg['arm']=='native' and cfg['architecture']=='x86'
    progress=base.read(parent/'PROGRESS.json') if (parent/'PROGRESS.json').exists() else {'state':{'calls':0},'rows':[]}
    rows=progress['rows'];calls=progress['state']['calls']
    assert calls==len(rows)<plan['model_response_cap']
    processed={r['attempt'] for r in rows};failed=[]
    for attempt in parent.glob('continuation-*'):
        if not attempt.is_dir() or str(attempt) in processed:continue
        probe=attempt/'checker_execution_probe';result=probe/'RESULT.json'
        if not result.exists():continue
        d=base.read(result)
        if not (d.get('execution_health')=='completed' and d.get('diagnostic_parity') and not d.get('trace_usable')):continue
        assert len(d['scans'])==4 and all(x['execution_valid'] for x in d['scans'])
        assert any(x.get('trace_capped') for x in d['scans'])
        receipt=base.read(probe/'RUN_MANIFEST.json')
        assert receipt['inputs_unchanged'] and all(base.sha(p)==h for p,h in receipt['inputs'].items())
        assert not (attempt/'llm_exchanges.jsonl').exists() and not (attempt/'RUN_MANIFEST.json').exists()
        wire=attempt/'provider_wire.jsonl'
        if wire.exists():assert wire.read_bytes()==b''
        failed.append(attempt)
    assert len(failed)==1, 'Ambiguous checkpoint requires explicit diagnosis'
    output=ROOT/'profile-repairs'/relative
    assert not output.exists()
    lane=PROJECT/'LLM-Native/SemWeaver/artifacts/external/linux-state-recovery-v41'
    lock=(ROOT/'.profile-cap-spare-kernel.lock').open('a+')
    fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
    output.mkdir(parents=True)
    cfg.update(linux_dir=str(lane),prefix_calls=calls,prefix_attempts=[r['attempt'] for r in rows])
    if calls:cfg['prefix_parent']=str(parent)
    base.save(output/'CONFIG.json',cfg)
    paths=[Path(__file__),ROOT/'run_any_profile_cap_repair.py',ROOT/'run_profile_cap_repair.py',
           ROOT/'profile_cap_entry.py',ROOT/'adaptive_trace_probe.py',ROOT/'fixed_trace_probe.py',output/'CONFIG.json',
           failed[0]/'checker_execution_probe/RESULT.json',failed[0]/'checker_execution_probe/RUN_MANIFEST.json']
    if calls:paths.append(parent/'PROGRESS.json')
    for row in rows:
        a=Path(row['attempt']);paths += [a/'RUN_MANIFEST.json',a/'SAGenTestChecker.cpp']
        if row['paired_valid']:paths.append(a/'frozen_validation/RESULT.json')
    inputs={**plan['inputs'],**{str(p):base.sha(p) for p in paths}}
    deadline=plan['deadline_epoch'];assert time.time()<deadline
    plan={**plan,'inputs':inputs,'parent':str(parent),'prefix_calls':calls,'started_at':time.time(),
          'operational_repair':'Same response ceiling/profile/method; complete uncapped diagnostic observations, all consumed reply prefixes retained.'}
    base.save(output/'RUN_PLAN.json',plan)
    prior=base.read(parent/'BUDGET.json');assert prior['dispatched_requests']>=calls
    base.save(output/'BUDGET.json',{'deadline_epoch':deadline,'max_requests':prior['max_requests']-prior['dispatched_requests'],
              'dispatched_requests':0,'dispatches':[],'prior_dispatches':prior['dispatched_requests']})
    source=PROJECT/'LLM-Native/SemWeaver-v43'
    key='/anonymous/home/.census-key' if cfg['profile']['provider']=='openai_compatible' else '/anonymous/home/.glm-key'
    command=['docker','run','--rm','--name','semweaver-profile-cap-spare',
             '-v',f'{PROJECT}:{PROJECT}','-v',f'{PROJECT/"LLM-Native"}:/work',
             '-v',f'{key}:/run/private/model-key:ro','-w','/work/SemWeaver-v43',
             '-e','PYTHONPATH=/work/SemWeaver-v43','-e','PATH=/usr/lib/llvm-18/bin:/usr/bin:/bin',
             '-e','GIT_CONFIG_COUNT=2','-e','GIT_CONFIG_KEY_0=safe.directory','-e',f'GIT_CONFIG_VALUE_0={lane}',
             '-e','GIT_CONFIG_KEY_1=safe.directory','-e',f'GIT_CONFIG_VALUE_1={source}',
             'sha256:2ddb870c297ffbb9b30b901f84da5542a8ae37eb8d6ce332eae1657fdd0014ec',
             '/work/SemWeaver/.venv-dev/bin/python',str(ROOT/'run_any_profile_cap_repair.py'),str(output/'CONFIG.json')]
    with (output/'container.stdout').open('x') as stdout,(output/'container.stderr').open('x') as stderr:
        code=subprocess.run(command,stdout=stdout,stderr=stderr,timeout=max(1,deadline-time.time())).returncode
    result=output/'RESULT.json';unchanged=all(base.sha(p)==h for p,h in inputs.items())
    base.save(output/'RUN_MANIFEST.json',{**plan,'status':'completed' if code==0 and unchanged and result.exists() else 'partial',
              'return_code':code,'inputs_unchanged':unchanged,'finished_at':time.time(),
              'result_sha256':base.sha(result) if result.exists() else None})
    print(json.dumps({'recovered_profile':str(relative),'return_code':code}),flush=True)
    lock.close()
