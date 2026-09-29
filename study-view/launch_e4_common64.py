"""Uniform larger-output robustness matrix; no truncated decode is zero effect."""
import fcntl
import hashlib
import json
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PROJECT = ROOT.parent
SOURCE = PROJECT / 'LLM-Native/SemWeaver-v43'
DATA = PROJECT / 'LLM-Native/artifacts/fse_revision'
LANE = PROJECT / 'LLM-Native/SemWeaver/artifacts/external/linux-e4-final-20260928'


def read(path): return json.loads(path.read_text())
def sha(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def save(path, data):
    with path.open('x') as handle:
        json.dump(data,handle,indent=2)
        handle.write('\n')


if __name__ == '__main__':
    capacity = read(ROOT/'LUNA_DIRECT_CAPACITY_SMOKE.json')
    assert capacity['rows'][0]['status']=='pass' and capacity['rows'][0]['ceiling']==65536
    preparation = read(ROOT/'E3_E4_PREPARATION.json')
    subjects = preparation['sampling_frame']
    deadline = time.time()+48*3600
    # The faulty16K scheduler was stopped between cells. Its current container
    # may still be returning a response; never share its tree or resend that call.
    while subprocess.check_output(['docker','ps','-q','--filter','name=^semweaver-e4-flash-'],text=True).strip():
        assert time.time()<deadline
        time.sleep(60)
    subprocess.run(['kill','-KILL','2633080'],capture_output=True)
    output = ROOT/'e4-common64'
    output.mkdir(exist_ok=False)
    profiles = [
        {'model':'glm-5.3-flash','provider':'anthropic','wire_api':'anthropic_messages','reasoning_effort':'','base_url':'https://open.bigmodel.cn/api/anthropic'},
        {'model':'glm-5.3','provider':'anthropic','wire_api':'anthropic_messages','reasoning_effort':'','base_url':'https://open.bigmodel.cn/api/anthropic'},
        {'model':'gpt-6-luna','provider':'openai_compatible','wire_api':'responses','reasoning_effort':'high','base_url':'https://model-gateway.example.invalid/v1'}]
    paths = [Path(__file__),ROOT/'run_profile_cell.py',ROOT/'run_profile_wire.py',ROOT/'provider_wire_entry.py',
             ROOT/'run_contextless_repair.py',ROOT/'contextless_probe.py',ROOT/'arm64_entry.py',
             ROOT/'LUNA_DIRECT_CAPACITY_SMOKE.json',ROOT/'GLM_PROFILE_SMOKE.json',ROOT/'E3_E4_PREPARATION.json',
             ROOT/'arm64-starting/RESULT.json']
    paths += [Path(path) for path in read(ROOT/'MATRIX_FREEZE.json')['common_input_hashes']]
    paths += [path for path in (DATA/'arm64_corrected_g04_v43').rglob('*') if path.is_file()]
    common = {str(path):sha(path) for path in paths}
    jobs = [(profile,1,case) for profile in profiles for case in subjects]
    jobs += [(profile,repeat,case) for profile in profiles for repeat in (2,3) for case in preparation['repeated_samples']]
    assert len(jobs)==189
    save(output/'MATRIX_FREEZE.json',{'source_revision':preparation['source_revision'],'profiles':profiles,
         'subjects':subjects,'repeated_samples':preparation['repeated_samples'],'planned_cells':189,
         'response_cap':2,'output_token_ceiling':65536,'deadline_epoch':deadline,'input_hashes':common,
         'superseded_configuration':'e4-flash-full39 used16K and returned a truncated reply before structured parsing; all original records remain.'})
    lock = (ROOT/'.e4-kernel.lock').open('a+')
    fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
    for profile,repeat,case in jobs:
        cell = output/profile['model']/f'repeat-{repeat}'/case
        cell.mkdir(parents=True,exist_ok=False)
        start_path = ROOT/'arm64-starting/RESULT.json' if case.startswith('G04_') else PROJECT/'native-trace-20260928/starting'/case/'RESULT.json'
        start = read(start_path)
        bundle = DATA/'arm64_corrected_g04_v43/csa/evidence_bundle.json' if case.startswith('G04_') else DATA/'generate_only_evidence_all39_v8_r5'/case/'csa/evidence_bundle.json'
        cfg = {'case_id':case,'arm':'native','profile':profile,'linux_dir':str(LANE),
               'starting_result':str(start_path),'evidence_bundle':str(bundle),
               'fixed_reports_dir':str(PROJECT/'native-trace-20260928/fixed_reports'/case/'fixed'),
               'objective':'precision_refine' if start['vulnerable_alerts'] else 'target_hit_recovery',
               'architecture':'arm64' if case.startswith('G04_') else 'x86','prior_adoptions':[]}
        save(cell/'CONFIG.json',cfg)
        inputs = {**common,str(cell/'CONFIG.json'):sha(cell/'CONFIG.json')}
        assert all(sha(path)==value for path,value in inputs.items())
        save(cell/'RUN_PLAN.json',{'inputs':inputs,'started_at':time.time(),'deadline_epoch':deadline,
             'model_response_cap':2,'output_token_ceiling':65536,'source_revision':preparation['source_revision']})
        save(cell/'BUDGET.json',{'deadline_epoch':deadline,'max_requests':16,'dispatched_requests':0,'dispatches':[]})
        key = '/anonymous/home/.census-key' if profile['model']=='gpt-6-luna' else '/anonymous/home/.glm-key'
        command = ['docker','run','--rm','--name',f"semweaver-e4-common-{profile['model']}-{repeat}-{case[:3]}",
             '-v',f'{PROJECT}:{PROJECT}','-v',f'{PROJECT/"LLM-Native"}:/work','-v',f'{key}:/run/private/model-key:ro',
             '-w','/work/SemWeaver-v43','-e','PYTHONPATH=/work/SemWeaver-v43',
             '-e','PATH=/usr/lib/llvm-18/bin:/usr/bin:/bin','-e','GIT_CONFIG_COUNT=2',
             '-e','GIT_CONFIG_KEY_0=safe.directory','-e',f'GIT_CONFIG_VALUE_0={LANE}',
             '-e','GIT_CONFIG_KEY_1=safe.directory','-e',f'GIT_CONFIG_VALUE_1={SOURCE}',
             'sha256:2ddb870c297ffbb9b30b901f84da5542a8ae37eb8d6ce332eae1657fdd0014ec',
             '/work/SemWeaver/.venv-dev/bin/python',str(ROOT/'run_profile_wire.py'),str(cell/'CONFIG.json')]
        with (cell/'container.stdout').open('x') as stdout,(cell/'container.stderr').open('x') as stderr:
            code = subprocess.run(command,stdout=stdout,stderr=stderr,timeout=max(1,deadline-time.time())).returncode
        result = cell/'RESULT.json'
        unchanged = all(sha(path)==value for path,value in inputs.items())
        save(cell/'RUN_MANIFEST.json',{'status':'completed' if code==0 and unchanged and result.exists() else 'partial',
             'return_code':code,'inputs':inputs,'inputs_unchanged':unchanged,'finished_at':time.time(),
             'result_sha256':sha(result) if result.exists() else None})
        print(json.dumps({'model':profile['model'],'repeat':repeat,'case':case,'return_code':code}),flush=True)
    lock.close()
