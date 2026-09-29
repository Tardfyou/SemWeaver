"""Consume completed baseline outputs, verify them without new model requests."""
import json
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PROJECT = ROOT.parent
SOURCE = PROJECT / 'LLM-Native/SemWeaver-v43'
LANE = PROJECT / 'LLM-Native/SemWeaver/artifacts/external/linux-knighter-replay-20260928'
IMAGE = 'sha256:2ddb870c297ffbb9b30b901f84da5542a8ae37eb8d6ce332eae1657fdd0014ec'


def read(path):
    try:
        return json.loads(path.read_text())
    except (FileNotFoundError, json.JSONDecodeError):
        return {}


if __name__ == '__main__':
    freeze = read(ROOT / 'knighter/MATRIX_FREEZE.json')
    deadline = freeze['deadline_epoch']
    while time.time() < deadline:
        completed = 0
        for case in freeze['subjects']:
            cell = ROOT / 'knighter' / case
            if (cell / 'PAIRED_RESULT.json').exists():
                assert read(cell / 'PAIRED_RESULT.json')['status'] == 'completed'
                completed += 1
                continue
            if read(cell / 'EXECUTION_MANIFEST.json').get('status') != 'completed_baseline_pending_paired_replay':
                continue
            if (cell / 'replay-container.stdout').exists():
                continue  # Interrupted validation requires checkpoint-specific recovery.
            command = ['docker', 'run', '--rm', '--name', 'semweaver-knighter-replay-' + case[:3],
                 '-v', f'{PROJECT}:{PROJECT}', '-v', f'{PROJECT/"LLM-Native"}:/work',
                 '-w', '/work/SemWeaver-v43', '-e', 'PYTHONPATH=/work/SemWeaver-v43',
                 '-e', 'PATH=/usr/lib/llvm-18/bin:/usr/bin:/bin', '-e', 'GIT_CONFIG_COUNT=2',
                 '-e', 'GIT_CONFIG_KEY_0=safe.directory', '-e', f'GIT_CONFIG_VALUE_0={LANE}',
                 '-e', 'GIT_CONFIG_KEY_1=safe.directory', '-e', f'GIT_CONFIG_VALUE_1={SOURCE}', IMAGE,
                 '/work/SemWeaver/.venv-dev/bin/python', str(ROOT / 'replay_baseline_cell.py'), str(cell)]
            print(json.dumps({'paired_replay_started': case, 'new_model_calls': 0}), flush=True)
            with (cell / 'replay-container.stdout').open('x') as stdout, (cell / 'replay-container.stderr').open('x') as stderr:
                code = subprocess.run(command, stdout=stdout, stderr=stderr,
                                      timeout=max(1, deadline-time.time())).returncode
            print(json.dumps({'paired_replay_finished': case, 'return_code': code}), flush=True)
        if completed == len(freeze['subjects']):
            break
        time.sleep(min(60, max(1, deadline-time.time())))
