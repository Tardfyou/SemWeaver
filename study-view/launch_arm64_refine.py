"""Rerun both G04 arms against the verified correct ARM64 input pair."""
import hashlib
import json
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PROJECT = ROOT.parent
SOURCE = PROJECT / 'LLM-Native/SemWeaver-v43'
CASE = 'G04_2e29b9971ac5_Null_Pointer_Dereference'
LANE = PROJECT / 'LLM-Native/SemWeaver/artifacts/external/linux-arm64-final-20260928'


def read(path):
    return json.loads(path.read_text())


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def save(path, data):
    with path.open('x') as handle:
        json.dump(data, handle, indent=2)
        handle.write('\n')


def main():
    start = read(ROOT / 'arm64-starting/RESULT.json')
    evidence = read(ROOT / 'arm64-evidence/EVIDENCE_REPLAY_MANIFEST.json')
    assert start['execution_valid'] and start['vulnerable_object_counts'] == {'arch/arm64/kernel/process.o': 0}
    assert evidence['eligible'] and evidence['analyzer_internal_records'] > 0
    freeze = read(ROOT / 'MATRIX_FREEZE.json')
    image = 'sha256:2ddb870c297ffbb9b30b901f84da5542a8ae37eb8d6ce332eae1657fdd0014ec'
    shared = dict(freeze['common_input_hashes'])
    for path in [Path(__file__), ROOT / 'arm64_entry.py', ROOT / 'run_arm64_refine.py',
                 ROOT / 'arm64-starting/RESULT.json', ROOT / 'ARM64_CORRECTION.md']:
        shared[str(path)] = sha(path)
    for path in (ROOT / 'arm64-evidence').rglob('*'):
        if path.is_file():
            shared[str(path)] = sha(path)
    output = ROOT / 'architecture-corrected'
    output.mkdir(exist_ok=False)
    for arm in ('native', 'no_internal'):
        cell = output / ('03-' + arm)
        cell.mkdir()
        cfg = read(ROOT / 'runs' / ('03-' + arm) / 'CONFIG.json')
        cfg.update(linux_dir=str(LANE), prefix_attempts=[], prefix_calls=0)
        save(cell / 'CONFIG.json', cfg)
        inputs = {**shared, str(cell / 'CONFIG.json'): sha(cell / 'CONFIG.json')}
        assert all(sha(path) == value for path, value in inputs.items())
        plan = {'task_version': 'final39-arm64-scope-repair', 'started_at': time.time(),
                'deadline_epoch': freeze['deadline_epoch'], 'inputs': inputs,
                'source_revision': freeze['source_revision'], 'case_id': CASE, 'arm': arm,
                'prefix_calls': 0, 'model_response_cap': 32, 'responses_per_attempt': 1, 'image': image,
                'parent': str(ROOT / 'runs' / ('03-' + arm)),
                'scope_repair': 'Prior x86 object was not the patched ARM64 source; both arms restart from the same original checker with real ARM64 0/0 and native evidence.'}
        save(cell / 'RUN_PLAN.json', plan)
        save(cell / 'BUDGET.json', {'deadline_epoch': freeze['deadline_epoch'], 'max_requests': 256,
             'dispatched_requests': 0, 'dispatches': []})
        command = ['docker', 'run', '--rm', '--name', 'semweaver-arm64-' + arm,
             '-v', f'{PROJECT}:{PROJECT}', '-v', f'{PROJECT/"LLM-Native"}:/work',
             '-v', '/anonymous/home/.census-key:/run/private/gpt-key:ro',
             '-w', '/work/SemWeaver-v43', '-e', 'PYTHONPATH=/work/SemWeaver-v43',
             '-e', 'PATH=/usr/lib/llvm-18/bin:/usr/bin:/bin', '-e', 'GIT_CONFIG_COUNT=2',
             '-e', 'GIT_CONFIG_KEY_0=safe.directory', '-e', f'GIT_CONFIG_VALUE_0={LANE}',
             '-e', 'GIT_CONFIG_KEY_1=safe.directory', '-e', f'GIT_CONFIG_VALUE_1={SOURCE}', image,
             '/work/SemWeaver/.venv-dev/bin/python', str(ROOT / 'run_arm64_refine.py'), str(cell / 'CONFIG.json')]
        with (cell / 'container.stdout').open('x') as stdout, (cell / 'container.stderr').open('x') as stderr:
            code = subprocess.run(command, stdout=stdout, stderr=stderr,
                                  timeout=max(1, freeze['deadline_epoch']-time.time())).returncode
        result = cell / 'RESULT.json'
        unchanged = all(sha(path) == value for path, value in inputs.items())
        save(cell / 'RUN_MANIFEST.json', {**plan, 'status': 'completed' if code == 0 and unchanged and result.exists() else 'partial',
             'return_code': code, 'inputs_unchanged': unchanged, 'finished_at': time.time(),
             'result_sha256': sha(result) if result.exists() else None})
        print(json.dumps({'case_id': CASE, 'arm': arm, 'return_code': code, 'inputs_unchanged': unchanged}), flush=True)


if __name__ == '__main__':
    main()
