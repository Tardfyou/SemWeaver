"""Repair a Docker-name collision before any model/experiment initialization."""
import fcntl
import json
import subprocess
import time
from pathlib import Path
import launch_contextless_repair as base

ROOT=Path(__file__).resolve().parent
PROJECT=ROOT.parent
parent=ROOT/'e4-explicit-high/glm-5.3/repeat-1/G01_0f8ca019544a_Buffer_Overflow'
if __name__=='__main__':
    m=base.read(parent/'RUN_MANIFEST.json');assert m['status']=='partial' and m['return_code']==125
    assert 'container name' in (parent/'container.stderr').read_text() and 'Conflict' in (parent/'container.stderr').read_text()
    assert base.read(parent/'BUDGET.json')['dispatched_requests']==0 and not (parent/'INITIAL_STATE.json').exists()
    plan=base.read(parent/'RUN_PLAN.json');assert all(base.sha(p)==h for p,h in plan['inputs'].items())
    cfg=base.read(parent/'CONFIG.json');assert cfg['profile']['reasoning_effort']=='high'
    output=ROOT/'profile-repairs'/parent.relative_to(ROOT);output.mkdir(parents=True,exist_ok=False)
    lane=PROJECT/'LLM-Native/SemWeaver/artifacts/external/linux-e4-repeat-glmflash-v21'
    lock=(PROJECT/'native-trace-20260928/.contextless-spare-kernel.lock').open('a+')
    fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
    cfg['linux_dir']=str(lane);base.save(output/'CONFIG.json',cfg)
    inputs={**plan['inputs'],str(Path(__file__)):base.sha(Path(__file__)),str(output/'CONFIG.json'):base.sha(output/'CONFIG.json')}
    plan={**plan,'inputs':inputs,'parent':str(parent),'started_at':time.time(),
          'operational_repair':'Unique Docker name and idle locked kernel lane; zero previous dispatches; identical profile, scorer,2-response ceiling.'}
    base.save(output/'RUN_PLAN.json',plan);base.save(output/'BUDGET.json',base.read(parent/'BUDGET.json'))
    source=PROJECT/'LLM-Native/SemWeaver-v43'
    cmd=['docker','run','--rm','--name','semweaver-explicit-high-setup-repair-G01',
         '-v',f'{PROJECT}:{PROJECT}','-v',f'{PROJECT/"LLM-Native"}:/work',
         '-v','/anonymous/home/.glm-key:/run/private/model-key:ro','-w','/work/SemWeaver-v43',
         '-e','PYTHONPATH=/work/SemWeaver-v43','-e','PATH=/usr/lib/llvm-18/bin:/usr/bin:/bin',
         '-e','GIT_CONFIG_COUNT=2','-e','GIT_CONFIG_KEY_0=safe.directory','-e',f'GIT_CONFIG_VALUE_0={lane}',
         '-e','GIT_CONFIG_KEY_1=safe.directory','-e',f'GIT_CONFIG_VALUE_1={source}',
         'sha256:2ddb870c297ffbb9b30b901f84da5542a8ae37eb8d6ce332eae1657fdd0014ec',
         '/work/SemWeaver/.venv-dev/bin/python',str(ROOT/'run_glm_high_cell.py'),str(output/'CONFIG.json')]
    with (output/'container.stdout').open('x') as stdout,(output/'container.stderr').open('x') as stderr:
        code=subprocess.run(cmd,stdout=stdout,stderr=stderr,timeout=max(1,plan['deadline_epoch']-time.time())).returncode
    result=output/'RESULT.json';unchanged=all(base.sha(p)==h for p,h in inputs.items())
    base.save(output/'RUN_MANIFEST.json',{**plan,'status':'completed' if code==0 and unchanged and result.exists() else 'partial',
              'return_code':code,'inputs_unchanged':unchanged,'finished_at':time.time(),
              'result_sha256':base.sha(result) if result.exists() else None})
    lock.close()
