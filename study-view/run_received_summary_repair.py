"""Original profile loop with charged prefix and one offline received edit."""
import ast
from pathlib import Path
import subprocess
import sys
import run_profile_cell as profile

ROOT = Path(__file__).resolve().parent


if __name__ == '__main__':
    cfg = profile.read(sys.argv[1])
    original_run = subprocess.run
    def run(command, *args, **kwargs):
        if isinstance(command, list) and len(command) > 1 and Path(command[1]).name == 'run_semweaver_treatment_case.py':
            command = [command[0], str(ROOT / 'replay_received_summary_entry.py'),
                       '--source-root', str(profile.SOURCE), '--recorded-wire', cfg['response_replay_wire'],
                       '--candidate-sha', cfg['response_replay_candidate_sha256'], '--', *command[2:]]
        return original_run(command, *args, **kwargs)
    subprocess.run = run
    helper = ROOT / 'run_profile_cap_repair.py'
    tree = ast.parse(helper.read_text())
    functions = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in ('restore_prefix', 'patched_source')]
    namespace = {'ROOT': ROOT, 'Path': Path, 'profile': profile}
    code = ast.unparse(ast.Module(body=functions, type_ignores=[]))
    assert code.count("parent / 'PROGRESS.json'") == 1
    code = code.replace("parent / 'PROGRESS.json'", "parent / 'PREFIX_PROGRESS.json'")
    exec(compile(code, str(helper) + '[received-summary-replay]', 'exec'), namespace)
    runner = {**profile.__dict__, '__name__': 'received_summary_replay', 'restore_prefix': namespace['restore_prefix']}
    exec(compile(namespace['patched_source'](), str(ROOT / 'run_profile_cell.py') + '[received-summary-replay]', 'exec'), runner)
    runner['cell'](Path(sys.argv[1]))
