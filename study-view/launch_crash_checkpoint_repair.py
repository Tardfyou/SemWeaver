"""Preserve five consumed responses and queue a fresh execution diagnosis."""
import fcntl
import json
import subprocess
import time
from pathlib import Path
import launch_contextless_repair as base

ROOT=Path(__file__).resolve().parent
PROJECT=ROOT.parent


def main():
    parent=ROOT/'runs/10-native'
    manifest=base.read(parent/'RUN_MANIFEST.json')
    assert manifest['status']=='partial' and manifest['inputs_unchanged']
    assert all(base.sha(p)==h for p,h in manifest['inputs'].items())
    progress=base.read(parent/'PROGRESS.json')
    assert progress['state']['calls']==4
    attempts=[r['attempt'] for r in progress['rows']]+[str(parent/'continuation-05')]
    assert sum(base.read(Path(a)/'RUN_MANIFEST.json')['model_calls_used'] for a in attempts)==5
    output=ROOT/'repairs/10-native'
    output.mkdir(exist_ok=False)
    lock=(PROJECT/'native-trace-20260928/.slot-2.lock').open('a+')
    fcntl.flock(lock,fcntl.LOCK_EX)
    deadline=base.read(ROOT/'MATRIX_FREEZE.json')['deadline_epoch']
    assert time.time()<deadline
    cfg=base.read(parent/'CONFIG.json')
    lane=PROJECT/'LLM-Native/SemWeaver/artifacts/external/linux-e4-repeat-gpt-v10'
    cfg.update(linux_dir=str(lane),prefix_attempts=attempts,prefix_calls=5,crashed_attempt=attempts[-1])
    base.save(output/'CONFIG.json',cfg)
    additions=[Path(__file__), ROOT/'run_crash_checkpoint_repair.py',ROOT/'run_observer_repaired_cell.py',
               ROOT/'repaired_treatment_entry.py',ROOT/'fixed_trace_probe.py',ROOT/'run_contextless_repair.py',
               ROOT/'contextless_probe.py',output/'CONFIG.json']
    for a in attempts:
        additions += [Path(a)/'RUN_MANIFEST.json',Path(a)/'SAGenTestChecker.cpp']
    additions += [Path(attempts[-1])/'frozen_validation/RESULT.json']
    inputs={**manifest['inputs'],**{str(p):base.sha(p) for p in additions}}
    plan={**base.read(parent/'RUN_PLAN.json'),'inputs':inputs,'parent':str(parent),
          'prefix_calls':5,'started_at':time.time(),'operational_repair':'Fresh hash-bound candidate-crash diagnosis, then model-only repair; no execution failure scored as zero.'}
    base.save(output/'RUN_PLAN.json',plan)
    dispatched=base.read(parent/'BUDGET.json')['dispatched_requests']
    base.save(output/'BUDGET.json',{'deadline_epoch':deadline,'max_requests':256-dispatched,
              'dispatched_requests':0,'dispatches':[],'prior_dispatches':dispatched})
    source=PROJECT/'LLM-Native/SemWeaver-v43'
    command=['docker','run','--rm','--name','semweaver-final-crash-repair-G11',
             '-v',f'{PROJECT}:{PROJECT}','-v',f'{PROJECT/"LLM-Native"}:/work',
             '-v','/anonymous/home/.census-key:/run/private/gpt-key:ro',
             '-w','/work/SemWeaver-v43','-e','PYTHONPATH=/work/SemWeaver-v43',
             '-e','PATH=/usr/lib/llvm-18/bin:/usr/bin:/bin','-e','GIT_CONFIG_COUNT=2',
             '-e','GIT_CONFIG_KEY_0=safe.directory','-e',f'GIT_CONFIG_VALUE_0={lane}',
             '-e','GIT_CONFIG_KEY_1=safe.directory','-e',f'GIT_CONFIG_VALUE_1={source}',plan['image'],
             '/work/SemWeaver/.venv-dev/bin/python',str(ROOT/'run_crash_checkpoint_repair.py'),str(output/'CONFIG.json')]
    with (output/'container.stdout').open('x') as stdout,(output/'container.stderr').open('x') as stderr:
        code=subprocess.run(command,stdout=stdout,stderr=stderr,timeout=max(1,deadline-time.time())).returncode
    result=output/'RESULT.json';unchanged=all(base.sha(p)==h for p,h in inputs.items())
    base.save(output/'RUN_MANIFEST.json',{**plan,'status':'completed' if code==0 and unchanged and result.exists() else 'partial',
              'return_code':code,'inputs_unchanged':unchanged,'finished_at':time.time(),
              'result_sha256':base.sha(result) if result.exists() else None})
    print(json.dumps({'case':'G11','return_code':code,'inputs_unchanged':unchanged}),flush=True)
    lock.close()


if __name__=='__main__':main()
