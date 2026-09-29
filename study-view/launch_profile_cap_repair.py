"""Recover the exact Flash G03 prefix and its zero-response capped probe."""
import fcntl
import json
import os
import subprocess
import time
from pathlib import Path
import launch_contextless_repair as base

ROOT=Path(__file__).resolve().parent
PROJECT=ROOT.parent


def main():
    case='G03_1f886a7bfb3f_Null_Pointer_Dereference'
    relative=Path('e4-common64/glm-5.3-flash/repeat-1')/case
    parent=ROOT/relative
    plan=base.read(parent/'RUN_PLAN.json')
    assert all(base.sha(p)==h for p,h in plan['inputs'].items())
    progress=base.read(parent/'PROGRESS.json');rows=progress['rows']
    assert progress['state']['calls']==1 and len(rows)==1
    attempt=Path(rows[0]['attempt'])
    assert base.sha(attempt/'RUN_MANIFEST.json')==rows[0]['manifest_sha256']
    manifest=base.read(attempt/'RUN_MANIFEST.json')
    assert manifest['model_calls_used']==1 and manifest['model']=='glm-5.3-flash'
    failed=parent/'continuation-02';probe=failed/'checker_execution_probe'
    result=base.read(probe/'RESULT.json');receipt=base.read(probe/'RUN_MANIFEST.json')
    assert result['execution_health']=='completed' and result['diagnostic_parity'] and not result['trace_usable']
    assert len(result['scans'])==4 and all(x['execution_valid'] for x in result['scans'])
    assert any(x['trace_capped'] for x in result['scans'])
    assert receipt['inputs_unchanged'] and all(base.sha(p)==h for p,h in receipt['inputs'].items())
    assert not (failed/'llm_exchanges.jsonl').exists()
    assert (failed/'provider_wire.jsonl').read_bytes()==b''
    assert not (failed/'RUN_MANIFEST.json').exists()
    assert base.sha(failed/'SAGenTestChecker.cpp')==manifest['candidate_sha256']
    assert base.read(parent/'BUDGET.json')['dispatched_requests']==1
    live=subprocess.check_output(['docker','ps','--format','{{.Names}}'],text=True).splitlines()
    assert 'semweaver-e4-common-glm-5.3-flash-1-G03' not in live
    # The parent serial queue is deliberately suspended for disk safety, so its
    # missing outer receipt is not overwritten by this independently bound cell.
    parent_state=(Path('/proc/3316300/stat')).read_text().split(') ',1)[1].split()[0]
    assert parent_state=='T'
    info=os.statvfs(ROOT);assert info.f_bavail*info.f_frsize>=64*2**30
    lock=(PROJECT/'native-trace-20260928/.slot-2.lock').open('a+')
    fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
    output=ROOT/'profile-repairs'/relative;output.mkdir(parents=True,exist_ok=False)
    cfg=base.read(parent/'CONFIG.json');assert cfg['architecture']=='x86'
    lane=PROJECT/'LLM-Native/SemWeaver/artifacts/external/linux-e4-repeat-gpt-v10'
    cfg.update(linux_dir=str(lane),prefix_parent=str(parent),prefix_calls=1,prefix_attempts=[str(attempt)])
    base.save(output/'CONFIG.json',cfg)
    additions=[Path(__file__),ROOT/'run_profile_cap_repair.py',ROOT/'profile_cap_entry.py',
               ROOT/'adaptive_trace_probe.py',ROOT/'fixed_trace_probe.py',output/'CONFIG.json',
               parent/'PROGRESS.json',attempt/'RUN_MANIFEST.json',attempt/'SAGenTestChecker.cpp',
               attempt/'frozen_validation/RESULT.json',probe/'RESULT.json',probe/'RUN_MANIFEST.json',
               failed/'provider_wire.jsonl',failed/'SAGenTestChecker.cpp']
    inputs={**plan['inputs'],**{str(p):base.sha(p) for p in additions}}
    deadline=plan['deadline_epoch']
    plan={**plan,'inputs':inputs,'parent':str(parent),'prefix_calls':1,'started_at':time.time(),
          'operational_repair':'Same frozen selected method/profile; preserve first received reply and paired result, extend only disposable observer capacity64K/256K, retain original2-response ceiling.'}
    base.save(output/'RUN_PLAN.json',plan)
    base.save(output/'BUDGET.json',{'deadline_epoch':deadline,'max_requests':15,
              'dispatched_requests':0,'dispatches':[],'prior_dispatches':1})
    source=PROJECT/'LLM-Native/SemWeaver-v43'
    command=['docker','run','--rm','--name','semweaver-profile-cap-repair-G03',
             '-v',f'{PROJECT}:{PROJECT}','-v',f'{PROJECT/"LLM-Native"}:/work',
             '-v','/anonymous/home/.glm-key:/run/private/model-key:ro','-w','/work/SemWeaver-v43',
             '-e','PYTHONPATH=/work/SemWeaver-v43','-e','PATH=/usr/lib/llvm-18/bin:/usr/bin:/bin',
             '-e','GIT_CONFIG_COUNT=2','-e','GIT_CONFIG_KEY_0=safe.directory','-e',f'GIT_CONFIG_VALUE_0={lane}',
             '-e','GIT_CONFIG_KEY_1=safe.directory','-e',f'GIT_CONFIG_VALUE_1={source}',
             'sha256:2ddb870c297ffbb9b30b901f84da5542a8ae37eb8d6ce332eae1657fdd0014ec',
             '/work/SemWeaver/.venv-dev/bin/python',str(ROOT/'run_profile_cap_repair.py'),str(output/'CONFIG.json')]
    with (output/'container.stdout').open('x') as stdout,(output/'container.stderr').open('x') as stderr:
        code=subprocess.run(command,stdout=stdout,stderr=stderr,timeout=max(1,deadline-time.time())).returncode
    final=output/'RESULT.json';unchanged=all(base.sha(p)==h for p,h in inputs.items())
    base.save(output/'RUN_MANIFEST.json',{**plan,'status':'completed' if code==0 and unchanged and final.exists() else 'partial',
              'inputs_unchanged':unchanged,'return_code':code,'finished_at':time.time(),
              'result_sha256':base.sha(final) if final.exists() else None})
    print(json.dumps({'profile_cap_repair':case,'return_code':code,'inputs_unchanged':unchanged}),flush=True)
    lock.close()


if __name__=='__main__':main()
