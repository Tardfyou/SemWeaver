"""Replay a strictly zero-response profile cell after an observer failure."""
import fcntl
import json
import subprocess
import sys
import time
from pathlib import Path
import launch_contextless_repair as base

ROOT=Path(__file__).resolve().parent
PROJECT=ROOT.parent


def main():
    parent=Path(sys.argv[1]).resolve()
    relative=parent.relative_to(ROOT)
    assert relative.parts[0] in ('e3-selected-repeats','e4-common64')
    manifest=base.read(parent/'RUN_MANIFEST.json')
    assert manifest['status']=='partial' and manifest['inputs_unchanged']
    plan=base.read(parent/'RUN_PLAN.json')
    assert all(base.sha(p)==h for p,h in plan['inputs'].items())
    assert not (parent/'PROGRESS.json').exists(), 'Consumed prefixes require explicit checkpoint recovery'
    assert not (parent/'RESULT.json').exists()
    for attempt in parent.glob('continuation-*'):
        if not attempt.is_dir():continue
        assert not (attempt/'llm_exchanges.jsonl').exists()
        receipt=attempt/'RUN_MANIFEST.json'
        if receipt.exists():assert base.read(receipt).get('model_calls_used',0)==0
        wire=attempt/'provider_wire.jsonl'
        if wire.exists():assert not any(json.loads(s)['event']=='response' for s in wire.read_text().splitlines())
    assert base.read(parent/'BUDGET.json')['dispatched_requests']==0
    errors=[base.read(p).get('error') for p in parent.glob('continuation-*/checker_execution_probe/RESULT.json')]
    assert 'Instrumented checker build failed' in errors
    logs=list(parent.glob('continuation-*/checker_execution_probe/trace_build/build_stdout_1.log'))
    assert any("no matching function for call to 'emit'" in p.read_text() and "to 'const clang::Stmt *'" in p.read_text() for p in logs)
    output=ROOT/'profile-repairs'/relative
    output.mkdir(parents=True,exist_ok=False)
    lock=(PROJECT/'native-trace-20260928/.slot-2.lock').open('a+')
    fcntl.flock(lock,fcntl.LOCK_EX)
    deadline=plan['deadline_epoch'];assert time.time()<deadline
    cfg=base.read(parent/'CONFIG.json');assert cfg['architecture']=='x86'
    lane=PROJECT/'LLM-Native/SemWeaver/artifacts/external/linux-e4-repeat-gpt-v10'
    cfg['linux_dir']=str(lane)
    base.save(output/'CONFIG.json',cfg)
    additions=[Path(__file__),ROOT/'run_profile_observer_repair.py',ROOT/'repaired_pipeline_entry.py',
               ROOT/'fixed_trace_probe.py',output/'CONFIG.json']
    inputs={**plan['inputs'],**{str(p):base.sha(p) for p in additions}}
    plan={**plan,'inputs':inputs,'parent':str(parent),'started_at':time.time(),
          'operational_repair':'Observer statement-name capture, zero consumed responses; unchanged model/profile/output ceiling/cadence/retention.'}
    base.save(output/'RUN_PLAN.json',plan)
    budget=base.read(parent/'BUDGET.json');base.save(output/'BUDGET.json',budget)
    source=PROJECT/'LLM-Native/SemWeaver-v43'
    key='/anonymous/home/.census-key' if cfg['profile']['provider']=='openai_compatible' else '/anonymous/home/.glm-key'
    image='sha256:2ddb870c297ffbb9b30b901f84da5542a8ae37eb8d6ce332eae1657fdd0014ec'
    command=['docker','run','--rm','--name','semweaver-profile-observer-repair',
             '-v',f'{PROJECT}:{PROJECT}','-v',f'{PROJECT/"LLM-Native"}:/work','-v',f'{key}:/run/private/model-key:ro',
             '-w','/work/SemWeaver-v43','-e','PYTHONPATH=/work/SemWeaver-v43','-e','PATH=/usr/lib/llvm-18/bin:/usr/bin:/bin',
             '-e','GIT_CONFIG_COUNT=2','-e','GIT_CONFIG_KEY_0=safe.directory','-e',f'GIT_CONFIG_VALUE_0={lane}',
             '-e','GIT_CONFIG_KEY_1=safe.directory','-e',f'GIT_CONFIG_VALUE_1={source}',image,
             '/work/SemWeaver/.venv-dev/bin/python',str(ROOT/'run_profile_observer_repair.py'),str(output/'CONFIG.json')]
    with (output/'container.stdout').open('x') as stdout,(output/'container.stderr').open('x') as stderr:
        code=subprocess.run(command,stdout=stdout,stderr=stderr,timeout=max(1,deadline-time.time())).returncode
    result=output/'RESULT.json';unchanged=all(base.sha(p)==h for p,h in inputs.items())
    base.save(output/'RUN_MANIFEST.json',{**plan,'return_code':code,'inputs_unchanged':unchanged,
              'status':'completed' if code==0 and unchanged and result.exists() else 'partial',
              'result_sha256':base.sha(result) if result.exists() else None,'finished_at':time.time()})
    lock.close()
    print(json.dumps({'profile_repair':str(relative),'return_code':code}),flush=True)


if __name__=='__main__':main()
