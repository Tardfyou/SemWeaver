"""Final zero-response repair using a canonical verified evidence archive."""
import hashlib
import json
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PROJECT = ROOT.parent
ARCHIVE = PROJECT / 'LLM-Native/artifacts/fse_revision/arm64_corrected_g04_v43'


def read(path):
    return json.loads(path.read_text())


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def save(path, value):
    with path.open('x') as handle:
        json.dump(value, handle, indent=2)
        handle.write('\n')


if __name__ == '__main__':
    parent = ROOT / 'architecture-corrected/03-native-witness'
    previous = read(parent / 'RUN_MANIFEST.json')
    attempt = read(parent / 'continuation-01/RUN_MANIFEST.json')
    assert previous['status'] == 'partial' and previous['inputs_unchanged']
    assert attempt['model_calls_used'] == 0
    assert attempt['error_message'] == 'Native raw path is outside the frozen evidence root'
    alive = subprocess.check_output(['docker', 'ps', '-q', '--filter', 'name=^semweaver-arm64-no_internal$'], text=True)
    assert not alive.strip(), 'Do not share the live ARM tree'
    cell = ROOT / 'architecture-corrected/03-native-bound'
    cell.mkdir(exist_ok=False)
    cfg = read(parent / 'CONFIG.json')
    save(cell / 'CONFIG.json', cfg)
    inputs = dict(previous['inputs'])
    for path in [Path(__file__), ROOT / 'run_arm64_refine_bound.py', ROOT / 'bind_arm64_archive.py', cell / 'CONFIG.json']:
        inputs[str(path)] = sha(path)
    for path in ARCHIVE.rglob('*'):
        if path.is_file(): inputs[str(path)] = sha(path)
    assert all(sha(path) == value for path, value in inputs.items())
    plan = {**read(parent / 'RUN_PLAN.json'), 'inputs': inputs, 'started_at': time.time(),
            'parent': str(parent), 'operational_repair': 'Actual raw files archived within the existing canonical fse_revision boundary; hashes retained.'}
    save(cell / 'RUN_PLAN.json', plan)
    deadline = previous['deadline_epoch']
    save(cell / 'BUDGET.json', {'deadline_epoch': deadline, 'max_requests': 256,
         'dispatched_requests': 0, 'dispatches': []})
    source = PROJECT / 'LLM-Native/SemWeaver-v43'
    command = ['docker', 'run', '--rm', '--name', 'semweaver-arm64-native-bound',
         '-v', f'{PROJECT}:{PROJECT}', '-v', f'{PROJECT/"LLM-Native"}:/work',
         '-v', '/anonymous/home/.census-key:/run/private/gpt-key:ro',
         '-w', '/work/SemWeaver-v43', '-e', 'PYTHONPATH=/work/SemWeaver-v43',
         '-e', 'PATH=/usr/lib/llvm-18/bin:/usr/bin:/bin', '-e', 'GIT_CONFIG_COUNT=2',
         '-e', 'GIT_CONFIG_KEY_0=safe.directory', '-e', f'GIT_CONFIG_VALUE_0={cfg["linux_dir"]}',
         '-e', 'GIT_CONFIG_KEY_1=safe.directory', '-e', f'GIT_CONFIG_VALUE_1={source}', previous['image'],
         '/work/SemWeaver/.venv-dev/bin/python', str(ROOT / 'run_arm64_refine_bound.py'), str(cell / 'CONFIG.json')]
    with (cell / 'container.stdout').open('x') as stdout, (cell / 'container.stderr').open('x') as stderr:
        code = subprocess.run(command, stdout=stdout, stderr=stderr, timeout=max(1, deadline-time.time())).returncode
    result = cell / 'RESULT.json'
    unchanged = all(sha(path) == value for path, value in inputs.items())
    save(cell / 'RUN_MANIFEST.json', {**plan, 'status': 'completed' if code == 0 and unchanged and result.exists() else 'partial',
         'return_code': code, 'inputs_unchanged': unchanged, 'finished_at': time.time(),
         'result_sha256': sha(result) if result.exists() else None})
    print(json.dumps({'return_code': code, 'inputs_unchanged': unchanged}), flush=True)
