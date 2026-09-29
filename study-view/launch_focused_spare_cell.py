"""One absent focused cell on a verified closed baseline/E3 kernel lane.

Scheduling-only adapter: original12, original model profile, two replies and
65536 output ceiling. No new cell, retry allowance or outcome selection.
"""
import ast
import fcntl
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

from resume_verified_queues import SkipExisting

ROOT = Path(__file__).resolve().parent


if __name__ == '__main__':
    model = sys.argv[1]
    assert model in ('glm-5.3', 'glm-5.3-flash')
    script = ROOT / 'launch_e4_common64.py'
    spec = importlib.util.spec_from_file_location('original_focused_spare', script)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    status = module.read(ROOT / 'REMAINING_MATRIX_STATUS.json')
    assert status['e3']['verified_complete'] == status['e3']['planned_new_cells'] == 48
    assert not any(r['integrity_errors'] for r in status['e3']['rows'])
    freeze = module.read(ROOT / 'e4-focused12' / model / 'MATRIX_FREEZE.json')
    assert freeze['subjects'] == module.read(ROOT / 'E3_E4_PREPARATION.json')['repeated_samples']
    assert freeze['profile']['model'] == model and freeze['profile']['reasoning_effort'] == 'high'
    assert freeze['response_cap'] == 2 and freeze['output_token_ceiling'] == 65536
    pending = [case for case in freeze['subjects'] if not (ROOT / 'e4-focused12' / model / 'repeat-1' / case).exists()]
    assert pending, 'No absent focused cell'
    lane = ROOT.parent / 'LLM-Native/SemWeaver/artifacts/external/linux-knighter-final-20260928'
    assert lane.is_dir() and not lane.is_symlink()
    kernel_lock = (ROOT / '.knighter-kernel.lock').open('a+')
    fcntl.flock(kernel_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    reuse_lock = (ROOT / '.e3-repeat3-native-kernel.lock').open('a+')
    fcntl.flock(reuse_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    live = subprocess.check_output(['docker', 'ps', '-q', '--filter', 'name=semweaver'], text=True).split()
    containers = json.loads(subprocess.check_output(['docker', 'inspect', *live], text=True)) if live else []
    assert not any(str(lane) in ' '.join((c['Config'].get('Cmd') or []) + (c['Config'].get('Env') or []))
                   for c in containers), 'Kernel lane is in use'
    case = pending[0]  # Earliest original-order absent case, no result inspection.
    common = {**freeze['input_hashes'], str(Path(__file__)): module.sha(Path(__file__)),
              str(ROOT / 'resume_verified_queues.py'): module.sha(ROOT / 'resume_verified_queues.py')}
    assert all(module.sha(path) == value for path, value in common.items())
    module.save(ROOT / f'FOCUSED_SPARE_{model}_{case[:3]}_PLAN.json', {
        'model': model, 'case_id': case, 'profile': freeze['profile'], 'kernel_lane': str(lane),
        'additional_cells': 0, 'additional_model_reply_allowance': 0,
        'original_focused_freeze_sha256': module.sha(ROOT / 'e4-focused12' / model / 'MATRIX_FREEZE.json'),
        'input_hashes': common, 'response_cap': 2, 'output_token_ceiling': 65536,
        'boundary': 'Scheduling one original absent cell only. Existing directories are never replaced or restarted.'})
    namespace = module.__dict__
    namespace.update(output=ROOT / 'e4-focused12', common=common, deadline=freeze['deadline_epoch'],
                     preparation=module.read(ROOT / 'E3_E4_PREPARATION.json'),
                     jobs=[(freeze['profile'], 1, case)], LANE=lane)
    tree = ast.parse(script.read_text())
    body = next(n.body for n in tree.body if isinstance(n, ast.If) and ast.unparse(n.test) == "__name__ == '__main__'")
    start = next(i for i, node in enumerate(body) if isinstance(node, ast.For))
    tail = ast.fix_missing_locations(SkipExisting().visit(ast.Module(body=body[start:], type_ignores=[])))
    original_run = subprocess.run
    def run(command, *args, **kwargs):
        if isinstance(command, list) and str(ROOT / 'run_profile_wire.py') in command:
            command = list(command)
            command[command.index(str(ROOT / 'run_profile_wire.py'))] = str(ROOT / 'run_glm_mapped_high_cell.py')
        if isinstance(command, list) and '--name' in command:
            command = list(command)
            i = command.index('--name') + 1
            command[i] = command[i].replace('semweaver-e4-common-', 'semweaver-focused-spare-')
        return original_run(command, *args, **kwargs)
    subprocess.run = run
    exec(compile(tail, str(script) + '[one-original-focused-cell-spare-lane]', 'exec'), namespace)
