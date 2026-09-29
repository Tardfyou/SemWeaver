"""Resume never-launched cells after the original scheduler becomes terminal."""
import concurrent.futures
import fcntl
import hashlib
import json
import queue
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PROJECT = ROOT.parent
SOURCE = PROJECT / 'LLM-Native/SemWeaver-v43'
DATA = PROJECT / 'LLM-Native/artifacts/fse_revision'
RUNNER = PROJECT / 'native-state-20260928/run_cell_v43.py'
TREES = ['linux-e3-repeat-native-v13', 'linux-e3-repeat-nointernal-v13',
         'linux-e4-repeat-gpt-v10', 'linux-e4-repeat-glm53-v21']


def read(path):
    return json.loads(path.read_text())


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def save(path, value):
    with path.open('x') as handle:
        json.dump(value, handle, indent=2)
        handle.write('\n')


def parent_live(pid):
    path = Path('/proc') / str(pid)
    try:
        args = (path / 'cmdline').read_bytes().split(b'\0')
        state = (path / 'stat').read_text().split(') ', 1)[1].split()[0]
        return state != 'Z' and b'final-native-20260928/launch_final.py' in args
    except FileNotFoundError:
        return False


if __name__ == '__main__':
    freeze = read(ROOT / 'MATRIX_FREEZE.json')
    deadline = freeze['deadline_epoch']
    while parent_live(1960194) and time.time() < deadline:
        time.sleep(min(60, max(1, deadline-time.time())))
    assert not parent_live(1960194), 'Never overlap a live scheduler'
    main_containers = subprocess.check_output(['docker', 'ps', '--format', '{{.Names}}'], text=True).splitlines()
    assert not any(name.startswith('semweaver-final-') and not name.startswith('semweaver-final-repair-') for name in main_containers)
    reused = {row['sample_id'] for row in freeze['reused_pairs']}
    items = [(i, case, arm) for i, case in enumerate(freeze['subjects']) if case not in reused
             for arm in ('native', 'no_internal') if not (ROOT / 'runs' / f'{i:02d}-{arm}').exists()]
    common = freeze['common_input_hashes']
    assert all(sha(path) == value for path, value in common.items())
    common = {**common, str(Path(__file__)): sha(__file__)}
    save(ROOT / 'MISSING_CELL_RESUME_PLAN.json', {'planned_missing_cells': len(items),
         'items': items, 'original_scheduler_pid': 1960194, 'scheduler_confirmed_terminal': True,
         'input_hashes': common, 'deadline_epoch': deadline,
         'boundary': 'Only absent output directories are launched. Existing live, completed or partial cells are never overwritten.'})
    slots = queue.Queue()
    for slot in (0, 1, 3): slots.put(slot)
    starts = {row['case_id']: row for row in read(DATA / 'scanfix_v26_corrected_summary_v1/RESULT.json')['starting']['rows']}

    def run(item):
        index, case, arm = item
        slot = slots.get()
        cell = ROOT / 'runs' / f'{index:02d}-{arm}'
        cell.mkdir(exist_ok=False)
        lock = (PROJECT / 'native-trace-20260928' / f'.slot-{slot}.lock').open('a+')
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            assert all(sha(path) == value for path, value in common.items())
            lane = PROJECT / 'LLM-Native/SemWeaver/artifacts/external' / TREES[slot]
            cfg = {'case_id': case, 'arm': arm, 'index': index, 'linux_dir': str(lane),
                   'objective': 'precision_refine' if starts[case]['vulnerable_alerts'] else 'target_hit_recovery',
                   'prefix_attempts': [], 'prefix_calls': 0, 'prior_adoptions': []}
            for path in (PROJECT / 'native-trace-20260928/adoption').glob('*/ADOPTION.json'):
                if read(path)['case_id'] == case:
                    cfg['prior_adoptions'].append(str(path))
                    break
            save(cell / 'CONFIG.json', cfg)
            inputs = {**common, str(cell / 'CONFIG.json'): sha(cell / 'CONFIG.json'),
                      str(ROOT / 'MATRIX_FREEZE.json'): sha(ROOT / 'MATRIX_FREEZE.json')}
            for name in cfg['prior_adoptions']:
                path = Path(name)
                inputs[str(path)] = sha(path)
                for row in read(path)['pool']:
                    inputs[row['quality_result']] = sha(row['quality_result'])
            plan = {'task_version': 'final39-never-launched-resume', 'started_at': time.time(),
                    'deadline_epoch': deadline, 'inputs': inputs, 'prefix_calls': 0,
                    'source_revision': freeze['source_revision'], 'case_id': case, 'arm': arm,
                    'model_response_cap': 32, 'responses_per_attempt': 1,
                    'image': 'sha256:2ddb870c297ffbb9b30b901f84da5542a8ae37eb8d6ce332eae1657fdd0014ec'}
            save(cell / 'RUN_PLAN.json', plan)
            save(cell / 'BUDGET.json', {'deadline_epoch': deadline, 'max_requests': 256,
                 'dispatched_requests': 0, 'dispatches': []})
            command = ['docker', 'run', '--rm', '--name', 'semweaver-final-' + cell.name,
                 '-v', f'{PROJECT}:{PROJECT}', '-v', f'{PROJECT/"LLM-Native"}:/work',
                 '-v', '/anonymous/home/.census-key:/run/private/gpt-key:ro',
                 '-w', '/work/SemWeaver-v43', '-e', 'PYTHONPATH=/work/SemWeaver-v43',
                 '-e', 'PATH=/usr/lib/llvm-18/bin:/usr/bin:/bin', '-e', 'GIT_CONFIG_COUNT=2',
                 '-e', 'GIT_CONFIG_KEY_0=safe.directory', '-e', f'GIT_CONFIG_VALUE_0={lane}',
                 '-e', 'GIT_CONFIG_KEY_1=safe.directory', '-e', f'GIT_CONFIG_VALUE_1={SOURCE}', plan['image'],
                 '/work/SemWeaver/.venv-dev/bin/python', str(RUNNER), str(cell / 'CONFIG.json')]
            with (cell / 'container.stdout').open('x') as stdout, (cell / 'container.stderr').open('x') as stderr:
                code = subprocess.run(command, stdout=stdout, stderr=stderr, timeout=max(1, deadline-time.time())).returncode
            result = cell / 'RESULT.json'
            unchanged = all(sha(path) == value for path, value in inputs.items())
            save(cell / 'RUN_MANIFEST.json', {**plan, 'status': 'completed' if code == 0 and unchanged and result.exists() else 'partial',
                 'return_code': code, 'inputs_unchanged': unchanged, 'finished_at': time.time(),
                 'result_sha256': sha(result) if result.exists() else None})
            print(json.dumps({'resumed_cell': cell.name, 'return_code': code, 'inputs_unchanged': unchanged}), flush=True)
            return code
        finally:
            lock.close()
            slots.put(slot)

    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
        results = list(pool.map(run, items))
    save(ROOT / 'MISSING_CELL_RESUME_RESULT.json', {'items': items, 'return_codes': results})
