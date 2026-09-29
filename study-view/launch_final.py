"""Run the remaining33 paired samples using the selected immutable protocol."""
import concurrent.futures
import fcntl
import json
import queue
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PROJECT = ROOT.parent
PREVIOUS = PROJECT / 'native-state-20260928'
SOURCE = PROJECT / 'LLM-Native/SemWeaver-v43'
DATA = PROJECT / 'LLM-Native/artifacts/fse_revision'
RUNNER = PREVIOUS / 'run_cell_v43.py'
IMAGE = 'sha256:2ddb870c297ffbb9b30b901f84da5542a8ae37eb8d6ce332eae1657fdd0014ec'
TREES = ['linux-e3-repeat-native-v13', 'linux-e3-repeat-nointernal-v13',
         'linux-e4-repeat-gpt-v10', 'linux-e4-repeat-glm53-v21']


def read(path):
    return json.loads(path.read_text())


def sha(path):
    import hashlib
    path = Path(path)
    if str(path).startswith('/work/'):
        path = PROJECT / 'LLM-Native' / str(path)[len('/work/'):]
    return hashlib.sha256(path.read_bytes()).hexdigest()


def save(path, data):
    with path.open('x') as handle:
        json.dump(data, handle, indent=2)
        handle.write('\n')


def main():
    selection = read(ROOT / 'SELECTION.json')
    assert sha(ROOT / 'PLAN.md') == selection['plan_sha256']
    revision = subprocess.check_output(['git', '-C', str(SOURCE), 'rev-parse', 'HEAD'], text=True).strip()
    assert revision == selection['source_revision']
    starts = read(DATA / 'scanfix_v26_corrected_summary_v1/RESULT.json')['starting']['rows']
    assert len(starts) == len({row['case_id'] for row in starts}) == 39
    reused = {row['sample_id']: row for row in selection['reused_pairs']}
    assert len(reused) == 6
    paths = [Path(__file__), ROOT / 'PLAN.md', ROOT / 'SELECTION.json', RUNNER,
             DATA / 'scanfix_v26_corrected_summary_v1/RESULT.json', SOURCE / 'config/config.yaml']
    for folder in ('src', 'prompts', 'experiments/knighter/experiment',
                   'experiments/knighter/baseline', 'tests/fixtures',
                   'experiments/robustness/case07_negative_control'):
        paths += [p for p in (SOURCE / folder).rglob('*')
                  if p.is_file() and '__pycache__' not in p.parts and p.suffix != '.pyc']
    for row in starts:
        case = row['case_id']
        paths += [p for p in (DATA / 'generate_only_materialized_v1/cases' / case).rglob('*') if p.is_file()]
        paths += [DATA / 'generate_only_evidence_all39_v8_r5' / case / 'csa/evidence_bundle.json',
                  PROJECT / 'native-trace-20260928/starting' / case / 'RESULT.json']
        paths += [p for p in (PROJECT / 'native-trace-20260928/fixed_reports' / case).rglob('*') if p.is_file()]
        if case in reused:
            for arm in ('native', 'no_internal'):
                old = PREVIOUS / reused[case][arm]['path']
                paths += [old / 'RUN_MANIFEST.json', old / 'RESULT.json']
                assert sha(old / 'RESULT.json') == reused[case][arm]['result_sha256']
    common = {str(p): sha(p) for p in paths}
    started = time.time()
    deadline = started + 24 * 3600
    items = [(index, row['case_id'], arm) for index, row in enumerate(starts)
             if row['case_id'] not in reused for arm in ('native', 'no_internal')]
    assert len(items) == 66
    OUT = ROOT / 'runs'
    OUT.mkdir(exist_ok=False)
    save(ROOT / 'MATRIX_FREEZE.json', {'started_at': started, 'deadline_epoch': deadline,
         'source_revision': revision, 'planned_samples': 39, 'planned_cells': 78,
         'reused_pairs': selection['reused_pairs'], 'new_cells': 66,
         'subjects': [r['case_id'] for r in starts], 'model': 'gpt-6-luna',
         'reasoning_effort': 'high', 'configured_temperature': 0, 'output_token_ceiling': 16384,
         'response_ceiling_per_cell': 32, 'responses_per_attempt': 1,
         'common_input_hashes': common, 'all_cases_are_experimental_samples': True})
    slots = queue.Queue()
    for slot in (0, 1, 3):
        slots.put(slot)
    starting = {row['case_id']: row for row in starts}

    def run(item):
        index, case, arm = item
        slot = slots.get()
        output = OUT / f'{index:02d}-{arm}'
        output.mkdir()
        lock = (PROJECT / 'native-trace-20260928' / f'.slot-{slot}.lock').open('a+')
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            assert all(sha(p) == h for p, h in common.items()), 'Frozen input drift'
            linux = PROJECT / 'LLM-Native/SemWeaver/artifacts/external' / TREES[slot]
            cfg = {'case_id': case, 'arm': arm, 'index': index, 'linux_dir': str(linux),
                   'objective': 'precision_refine' if starting[case]['vulnerable_alerts'] else 'target_hit_recovery',
                   'prefix_attempts': [], 'prefix_calls': 0, 'prior_adoptions': []}
            for path in (PROJECT / 'native-trace-20260928/adoption').glob('*/ADOPTION.json'):
                if read(path)['case_id'] == case:
                    cfg['prior_adoptions'].append(str(path))
                    break
            save(output / 'CONFIG.json', cfg)
            inputs = {**common, str(output / 'CONFIG.json'): sha(output / 'CONFIG.json'),
                      str(ROOT / 'MATRIX_FREEZE.json'): sha(ROOT / 'MATRIX_FREEZE.json')}
            for name in cfg['prior_adoptions']:
                path = Path(name)
                inputs[str(path)] = sha(path)
                for row in read(path)['pool']:
                    path = Path(row['quality_result'])
                    inputs[str(path)] = sha(path)
            plan = {'task_version': 'final39-selected-v43', 'started_at': time.time(),
                    'deadline_epoch': deadline, 'inputs': inputs, 'prefix_calls': 0,
                    'source_revision': revision, 'case_id': case, 'arm': arm,
                    'model_response_cap': 32, 'responses_per_attempt': 1, 'image': IMAGE}
            save(output / 'RUN_PLAN.json', plan)
            save(output / 'BUDGET.json', {'deadline_epoch': deadline, 'max_requests': 256,
                 'dispatched_requests': 0, 'dispatches': []})
            name = 'semweaver-final-' + output.name
            command = ['docker', 'run', '--rm', '--name', name,
                '-v', f'{PROJECT}:{PROJECT}', '-v', f'{PROJECT/"LLM-Native"}:/work',
                '-v', '/anonymous/home/.census-key:/run/private/gpt-key:ro',
                '-w', '/work/SemWeaver-v43', '-e', 'PYTHONPATH=/work/SemWeaver-v43',
                '-e', 'PATH=/usr/lib/llvm-18/bin:/usr/bin:/bin',
                '-e', 'GIT_CONFIG_COUNT=2', '-e', 'GIT_CONFIG_KEY_0=safe.directory',
                '-e', f'GIT_CONFIG_VALUE_0={linux}', '-e', 'GIT_CONFIG_KEY_1=safe.directory',
                '-e', f'GIT_CONFIG_VALUE_1={SOURCE}', IMAGE,
                '/work/SemWeaver/.venv-dev/bin/python', str(RUNNER), str(output / 'CONFIG.json')]
            code = 124
            if time.time() < deadline:
                with (output / 'container.stdout').open('x') as stdout, (output / 'container.stderr').open('x') as stderr:
                    try:
                        code = subprocess.run(command, stdout=stdout, stderr=stderr,
                                              timeout=max(1, deadline-time.time())).returncode
                    except subprocess.TimeoutExpired:
                        subprocess.run(['docker', 'stop', '--time', '5', name], capture_output=True, timeout=20)
            unchanged = all(sha(p) == h for p, h in inputs.items())
            result = output / 'RESULT.json'
            complete = code == 0 and unchanged and result.exists()
            save(output / 'RUN_MANIFEST.json', {**plan, 'status': 'completed' if complete else 'partial',
                 'return_code': code, 'inputs_unchanged': unchanged, 'finished_at': time.time(),
                 'result_sha256': sha(result) if result.exists() else None})
            outcome = {'case_id': case, 'arm': arm, 'cell': output.name, 'complete': complete, 'return_code': code}
            print(json.dumps(outcome), flush=True)
            return outcome
        finally:
            lock.close()
            slots.put(slot)

    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
        rows = list(pool.map(run, items))
    save(ROOT / 'EXECUTION_SUMMARY.json', {'planned_samples': 39, 'reused_pairs': 6,
         'new_cells': 66, 'all_complete': all(row['complete'] for row in rows), 'rows': rows})


if __name__ == '__main__':
    main()
