"""Full39 Flash robustness cells under the explicit shared two-response cap."""
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
def save(path, value):
    with path.open('x') as handle:
        json.dump(value, handle, indent=2)
        handle.write('\n')


if __name__ == '__main__':
    smoke = read(ROOT / 'GLM_PROFILE_SMOKE.json')
    assert all(row['status'] == 'pass' for row in smoke['rows'])
    preparation = read(ROOT / 'E3_E4_PREPARATION.json')
    subjects = preparation['sampling_frame']
    assert len(subjects) == 39
    output = ROOT / 'e4-flash-full39'
    output.mkdir(exist_ok=False)
    profile = {'model': 'glm-5.3-flash', 'provider': 'anthropic',
               'wire_api': 'anthropic_messages', 'reasoning_effort': '',
               'base_url': 'https://open.bigmodel.cn/api/anthropic'}
    paths = [Path(__file__), ROOT / 'run_profile_cell.py', ROOT / 'run_contextless_repair.py',
             ROOT / 'contextless_probe.py', ROOT / 'arm64_entry.py', ROOT / 'GLM_PROFILE_SMOKE.json',
             ROOT / 'E3_E4_PREPARATION.json', ROOT / 'arm64-starting/RESULT.json']
    paths += [Path(path) for path in read(ROOT / 'MATRIX_FREEZE.json')['common_input_hashes']]
    paths += [path for path in (DATA / 'arm64_corrected_g04_v43').rglob('*') if path.is_file()]
    common = {str(path): sha(path) for path in paths}
    deadline = time.time() + 24*3600
    save(output / 'MATRIX_FREEZE.json', {'subjects': subjects, 'planned_cases': 39, 'profile': profile,
         'response_ceiling': 2, 'output_token_ceiling': 16384, 'responses_per_attempt': 1,
         'deadline_epoch': deadline, 'input_hashes': common,
         'source_revision': preparation['source_revision'], 'all_cases_are_experimental_samples': True})
    lock = (ROOT / '.e4-kernel.lock').open('a+')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    for index, case in enumerate(subjects):
        cell = output / case
        cell.mkdir()
        start_path = ROOT / 'arm64-starting/RESULT.json' if case.startswith('G04_') else PROJECT / 'native-trace-20260928/starting' / case / 'RESULT.json'
        start = read(start_path)
        bundle = DATA / 'arm64_corrected_g04_v43/csa/evidence_bundle.json' if case.startswith('G04_') else DATA / 'generate_only_evidence_all39_v8_r5' / case / 'csa/evidence_bundle.json'
        cfg = {'case_id': case, 'arm': 'native', 'profile': profile, 'linux_dir': str(LANE),
               'starting_result': str(start_path), 'evidence_bundle': str(bundle),
               'fixed_reports_dir': str(PROJECT / 'native-trace-20260928/fixed_reports' / case / 'fixed'),
               'objective': 'precision_refine' if start['vulnerable_alerts'] else 'target_hit_recovery',
               'architecture': 'arm64' if case.startswith('G04_') else 'x86', 'prior_adoptions': []}
        save(cell / 'CONFIG.json', cfg)
        inputs = {**common, str(cell / 'CONFIG.json'): sha(cell / 'CONFIG.json')}
        assert all(sha(path) == value for path, value in inputs.items())
        save(cell / 'RUN_PLAN.json', {'inputs': inputs, 'started_at': time.time(), 'deadline_epoch': deadline,
             'model_response_cap': 2, 'output_token_ceiling': 16384, 'source_revision': preparation['source_revision']})
        save(cell / 'BUDGET.json', {'deadline_epoch': deadline, 'max_requests': 16,
             'dispatched_requests': 0, 'dispatches': []})
        command = ['docker', 'run', '--rm', '--name', 'semweaver-e4-flash-' + case[:3],
             '-v', f'{PROJECT}:{PROJECT}', '-v', f'{PROJECT/"LLM-Native"}:/work',
             '-v', '/anonymous/home/.glm-key:/run/private/model-key:ro',
             '-w', '/work/SemWeaver-v43', '-e', 'PYTHONPATH=/work/SemWeaver-v43',
             '-e', 'PATH=/usr/lib/llvm-18/bin:/usr/bin:/bin', '-e', 'GIT_CONFIG_COUNT=2',
             '-e', 'GIT_CONFIG_KEY_0=safe.directory', '-e', f'GIT_CONFIG_VALUE_0={LANE}',
             '-e', 'GIT_CONFIG_KEY_1=safe.directory', '-e', f'GIT_CONFIG_VALUE_1={SOURCE}',
             'sha256:2ddb870c297ffbb9b30b901f84da5542a8ae37eb8d6ce332eae1657fdd0014ec',
             '/work/SemWeaver/.venv-dev/bin/python', str(ROOT / 'run_profile_cell.py'), str(cell / 'CONFIG.json')]
        with (cell / 'container.stdout').open('x') as stdout, (cell / 'container.stderr').open('x') as stderr:
            code = subprocess.run(command, stdout=stdout, stderr=stderr, timeout=max(1, deadline-time.time())).returncode
        result = cell / 'RESULT.json'
        unchanged = all(sha(path) == value for path, value in inputs.items())
        save(cell / 'RUN_MANIFEST.json', {'status': 'completed' if code == 0 and unchanged and result.exists() else 'partial',
             'return_code': code, 'inputs': inputs, 'inputs_unchanged': unchanged, 'finished_at': time.time(),
             'result_sha256': sha(result) if result.exists() else None})
        print(json.dumps({'case': case, 'return_code': code, 'inputs_unchanged': unchanged}), flush=True)
    lock.close()
