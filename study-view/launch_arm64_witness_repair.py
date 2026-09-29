"""Wait for the live ARM peer, then repair zero-response raw-root binding."""
import hashlib
import json
import subprocess
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
    parent = ROOT / 'architecture-corrected/03-native'
    previous = read(parent / 'RUN_MANIFEST.json')
    attempt = read(parent / 'continuation-01/RUN_MANIFEST.json')
    assert previous['status'] == 'partial' and previous['inputs_unchanged']
    assert attempt['model_calls_used'] == 0 and attempt['error_message'] == 'Native raw path is outside the frozen evidence root'
    assert not (parent / 'continuation-01/llm_exchanges.jsonl').exists()
    deadline = previous['deadline_epoch']
    while time.time() < deadline:
        alive = subprocess.check_output(['docker', 'ps', '-q', '--filter', 'name=^semweaver-arm64-no_internal$'], text=True).strip()
        if not alive:
            break
        time.sleep(min(60, max(1, deadline-time.time())))
    cell = ROOT / 'architecture-corrected/03-native-witness'
    cell.mkdir(exist_ok=False)
    cfg = read(parent / 'CONFIG.json')
    save(cell / 'CONFIG.json', cfg)
    inputs = {**previous['inputs']}
    for path in [Path(__file__), ROOT / 'run_arm64_refine_witness.py', cell / 'CONFIG.json']:
        inputs[str(path)] = sha(path)
    assert all(sha(path) == value for path, value in inputs.items())
    plan = {**read(parent / 'RUN_PLAN.json'), 'started_at': time.time(), 'inputs': inputs,
            'parent': str(parent), 'operational_repair': 'Raw-path allowlist now binds the actual fresh ARM64 evidence root; zero consumed responses, no fabricated evidence.'}
    save(cell / 'RUN_PLAN.json', plan)
    save(cell / 'BUDGET.json', {'deadline_epoch': deadline, 'max_requests': 256,
                              'dispatched_requests': 0, 'dispatches': []})
    lane = cfg['linux_dir']
    source = PROJECT / 'LLM-Native/SemWeaver-v43'
    image = previous['image']
    command = ['docker', 'run', '--rm', '--name', 'semweaver-arm64-native-witness',
          '-v', f'{PROJECT}:{PROJECT}', '-v', f'{PROJECT/"LLM-Native"}:/work',
          '-v', '/anonymous/home/.census-key:/run/private/gpt-key:ro',
          '-w', '/work/SemWeaver-v43', '-e', 'PYTHONPATH=/work/SemWeaver-v43',
          '-e', 'PATH=/usr/lib/llvm-18/bin:/usr/bin:/bin', '-e', 'GIT_CONFIG_COUNT=2',
          '-e', 'GIT_CONFIG_KEY_0=safe.directory', '-e', f'GIT_CONFIG_VALUE_0={lane}',
          '-e', 'GIT_CONFIG_KEY_1=safe.directory', '-e', f'GIT_CONFIG_VALUE_1={source}', image,
          '/work/SemWeaver/.venv-dev/bin/python', str(ROOT / 'run_arm64_refine_witness.py'), str(cell / 'CONFIG.json')]
    with (cell / 'container.stdout').open('x') as stdout, (cell / 'container.stderr').open('x') as stderr:
        code = subprocess.run(command, stdout=stdout, stderr=stderr, timeout=max(1, deadline-time.time())).returncode
    result = cell / 'RESULT.json'
    unchanged = all(sha(path) == value for path, value in inputs.items())
    save(cell / 'RUN_MANIFEST.json', {**plan, 'status': 'completed' if code == 0 and unchanged and result.exists() else 'partial',
         'return_code': code, 'inputs_unchanged': unchanged, 'finished_at': time.time(),
         'result_sha256': sha(result) if result.exists() else None})
    print(json.dumps({'ARM_native_repair_return_code': code, 'inputs_unchanged': unchanged}), flush=True)


if __name__ == '__main__':
    main()
