"""Equal-ceiling actual KNighter runs with immutable model-call records."""
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
LANE = PROJECT / 'LLM-Native/SemWeaver/artifacts/external/linux-knighter-final-20260928'
IMAGE = 'sha256:2ddb870c297ffbb9b30b901f84da5542a8ae37eb8d6ce332eae1657fdd0014ec'


def read(path):
    return json.loads(path.read_text())


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def save(path, value):
    with path.open('x') as handle:
        json.dump(value, handle, indent=2)
        handle.write('\n')


def main():
    audit = read(ROOT / 'BASELINE_NATURAL_REUSE_AUDIT.json')
    assert all(row['reuse_eligible'] for row in audit['rows'])
    reused = {row['case_id'] for row in audit['rows']}
    starting = read(DATA / 'scanfix_v26_corrected_summary_v1/RESULT.json')['starting']['rows']
    subjects = [row['case_id'] for row in starting if row['fixed_alerts'] and row['case_id'] not in reused]
    assert len(subjects) == 13
    output = ROOT / 'knighter'
    output.mkdir(exist_ok=False)
    paths = [Path(__file__), ROOT / 'run_recorded_baseline.py', ROOT / 'BASELINE_NATURAL_REUSE_AUDIT.json',
             PROJECT / 'native-state-20260928/record_knighter_exchanges.py']
    for folder in ('experiments/knighter/baseline', 'experiments/knighter/experiment',
                   'experiments/robustness/case07_negative_control'):
        paths += [p for p in (SOURCE / folder).rglob('*') if p.is_file() and '__pycache__' not in p.parts and p.suffix != '.pyc']
    for case in subjects:
        paths += [p for p in (DATA / 'generate_only_materialized_v1/cases' / case).rglob('*') if p.is_file()]
        paths += [p for p in (PROJECT / 'native-trace-20260928/fixed_reports' / case).rglob('*') if p.is_file()]
    common = {str(path): sha(path) for path in paths}
    deadline = read(ROOT / 'MATRIX_FREEZE.json')['deadline_epoch']
    save(output / 'MATRIX_FREEZE.json', {'subjects': subjects, 'new_cases': 13,
         'reused_natural_cases': audit['rows'], 'no_fixed_report_cases': 24,
         'model': 'gpt-6-luna', 'reasoning_effort': 'high', 'response_ceiling': 32,
         'max_tokens': 16384, 'outer_max_tries': 2, 'max_fp_reports': 5,
         'deadline_epoch': deadline, 'input_hashes': common,
         'scope': 'Actual report-refinement algorithm. G04 baseline no-report behavior is bound to corrected ARM64 start, not old x86 validation.'})
    lock = (ROOT / '.knighter-kernel.lock').open('a+')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    for case in subjects:
        cell = output / case
        cell.mkdir()
        assert all(sha(path) == value for path, value in common.items())
        save(cell / 'RUN_PLAN.json', {'case_id': case, 'started_at': time.time(),
             'inputs': common, 'source_revision': read(ROOT / 'SELECTION.json')['source_revision'],
             'model_response_cap': 32, 'deadline_epoch': deadline})
        command = ['docker', 'run', '--rm', '--name', 'semweaver-knighter-' + case[:3],
             '-v', f'{PROJECT}:{PROJECT}', '-v', f'{PROJECT/"LLM-Native"}:/work',
             '-v', '/anonymous/home/.census-key:/run/private/gpt-key:ro',
             '-w', '/work/SemWeaver-v43', '-e', 'PYTHONPATH=/work/SemWeaver-v43',
             '-e', 'PATH=/usr/lib/llvm-18/bin:/usr/bin:/bin', '-e', 'GIT_CONFIG_COUNT=2',
             '-e', 'GIT_CONFIG_KEY_0=safe.directory', '-e', f'GIT_CONFIG_VALUE_0={LANE}',
             '-e', 'GIT_CONFIG_KEY_1=safe.directory', '-e', f'GIT_CONFIG_VALUE_1={SOURCE}', IMAGE,
             '/work/SemWeaver/.venv-dev/bin/python', str(ROOT / 'run_recorded_baseline.py'),
             '--source-root', str(SOURCE), '--exchange-log', str(cell / 'exchanges.jsonl'), '--',
             '--case-dir', str(DATA / 'generate_only_materialized_v1/cases' / case),
             '--fixed-report-dir', str(PROJECT / 'native-trace-20260928/fixed_reports' / case / 'fixed'),
             '--linux-dir', str(LANE), '--output-root', str(cell / 'output'),
             '--backend-workspace', str(cell / 'backend'), '--model', 'gpt-6-luna',
             '--reasoning-effort', 'high', '--temperature', '0', '--max-tokens', '16384',
             '--max-model-calls', '32', '--max-tries', '2', '--max-fp-reports', '5', '--jobs', '4']
        with (cell / 'container.stdout').open('x') as stdout, (cell / 'container.stderr').open('x') as stderr:
            code = subprocess.run(command, stdout=stdout, stderr=stderr,
                                  timeout=max(1, deadline-time.time())).returncode
        health = read(cell / 'exchanges.health.json') if (cell / 'exchanges.health.json').exists() else {}
        unchanged = all(sha(path) == value for path, value in common.items())
        save(cell / 'EXECUTION_MANIFEST.json', {'case_id': case, 'return_code': code,
             'status': 'completed_baseline_pending_paired_replay' if code == 0 and unchanged and health.get('status') == 'completed' else 'partial_unscored',
             'inputs_unchanged': unchanged, 'finished_at': time.time(),
             'model_responses': health.get('consumed_model_responses'),
             'health_sha256': sha(cell / 'exchanges.health.json') if health else None})
        print(json.dumps({'case_id': case, 'return_code': code, 'inputs_unchanged': unchanged}), flush=True)
    lock.close()


if __name__ == '__main__':
    main()
