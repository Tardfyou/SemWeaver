"""Resume a failed cell with a zero-response unsupported-context checkpoint."""
import fcntl
import hashlib
import json
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PROJECT = ROOT.parent


def read(path):
    return json.loads(path.read_text())


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def save(path, value):
    with path.open('x') as handle:
        json.dump(value, handle, indent=2)
        handle.write('\n')


def main():
    name = sys.argv[1]
    parent = ROOT / 'runs' / name
    manifest = read(parent / 'RUN_MANIFEST.json')
    assert manifest['status'] == 'partial' and manifest['inputs_unchanged']
    assert all(sha(path) == digest for path, digest in manifest['inputs'].items())
    progress = read(parent / 'PROGRESS.json') if (parent / 'PROGRESS.json').exists() else {'rows': [], 'state': {'calls': 0}}
    cfg = read(parent / 'CONFIG.json')
    cfg['prefix_attempts'] = [row['attempt'] for row in progress['rows']]
    cfg['prefix_calls'] = progress['state']['calls']
    lane = PROJECT / 'LLM-Native/SemWeaver/artifacts/external/linux-e4-repeat-gpt-v10'
    cfg['linux_dir'] = str(lane)
    output = ROOT / 'repairs' / name
    output.mkdir(parents=True, exist_ok=False)
    lock = (PROJECT / 'native-trace-20260928/.slot-2.lock').open('a+')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    save(output / 'CONFIG.json', cfg)
    additions = [Path(__file__), ROOT / 'contextless_probe.py', ROOT / 'run_contextless_repair.py', output / 'CONFIG.json']
    inputs = {**manifest['inputs'], **{str(path): sha(path) for path in additions}}
    deadline = read(ROOT / 'MATRIX_FREEZE.json')['deadline_epoch']
    plan = {**read(parent / 'RUN_PLAN.json'), 'started_at': time.time(),
            'inputs': inputs, 'parent': str(parent), 'prefix_calls': cfg['prefix_calls'],
            'operational_repair': 'Unsupported observer sites require fresh byte-identical four-scan parity before static-native fallback.'}
    save(output / 'RUN_PLAN.json', plan)
    dispatched = read(parent / 'BUDGET.json')['dispatched_requests']
    save(output / 'BUDGET.json', {'deadline_epoch': deadline, 'max_requests': 256-dispatched,
         'dispatched_requests': 0, 'dispatches': [], 'prior_dispatches': dispatched})
    source = PROJECT / 'LLM-Native/SemWeaver-v43'
    image = plan['image']
    command = ['docker', 'run', '--rm', '--name', 'semweaver-final-repair-' + name,
               '-v', f'{PROJECT}:{PROJECT}', '-v', f'{PROJECT/"LLM-Native"}:/work',
               '-v', '/anonymous/home/.census-key:/run/private/gpt-key:ro',
               '-w', '/work/SemWeaver-v43', '-e', 'PYTHONPATH=/work/SemWeaver-v43',
               '-e', 'PATH=/usr/lib/llvm-18/bin:/usr/bin:/bin',
               '-e', 'GIT_CONFIG_COUNT=2', '-e', 'GIT_CONFIG_KEY_0=safe.directory',
               '-e', f'GIT_CONFIG_VALUE_0={lane}', '-e', 'GIT_CONFIG_KEY_1=safe.directory',
               '-e', f'GIT_CONFIG_VALUE_1={source}', image,
               '/work/SemWeaver/.venv-dev/bin/python', str(ROOT / 'run_contextless_repair.py'), str(output / 'CONFIG.json')]
    with (output / 'container.stdout').open('x') as stdout, (output / 'container.stderr').open('x') as stderr:
        code = subprocess.run(command, stdout=stdout, stderr=stderr, timeout=max(1, deadline-time.time())).returncode
    result = output / 'RESULT.json'
    unchanged = all(sha(path) == digest for path, digest in inputs.items())
    save(output / 'RUN_MANIFEST.json', {**plan, 'status': 'completed' if code == 0 and unchanged and result.exists() else 'partial',
         'return_code': code, 'inputs_unchanged': unchanged, 'finished_at': time.time(),
         'result_sha256': sha(result) if result.exists() else None})
    print(json.dumps({'cell': name, 'return_code': code, 'inputs_unchanged': unchanged}), flush=True)
    lock.close()


if __name__ == '__main__':
    main()
