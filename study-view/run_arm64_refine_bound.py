"""Selected algorithm with architecture and canonical archive bindings only."""
import importlib.util
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PROJECT = ROOT.parent
spec = importlib.util.spec_from_file_location('arm64_final_bound_runner', PROJECT / 'native-state-20260928/run_cell_v43.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
original = runner.invoke


def invoke(command, output, env=None, timeout=None):
    command = list(command)
    script = Path(command[1]).name if len(command) > 1 else ''
    if script == 'run_semweaver_treatment_case.py':
        command[command.index('--evidence-bundle') + 1] = str(PROJECT / 'LLM-Native/artifacts/fse_revision/arm64_corrected_g04_v43/csa/evidence_bundle.json')
        if '--feedback-result' in command:
            index = command.index('--feedback-result') + 1
            if '/native-trace-20260928/starting/' in command[index]:
                command[index] = str(ROOT / 'arm64-starting/RESULT.json')
    if script in ('run_semweaver_treatment_case.py', 'validate_frozen_csa_candidate.py'):
        command = [command[0], str(ROOT / 'arm64_entry.py'), '--source-root', str(runner.SOURCE),
                   '--script', script, '--', *command[2:]]
    return original(command, output, env, timeout)


runner.invoke = invoke
if __name__ == '__main__':
    runner.cell(Path(sys.argv[1]))
