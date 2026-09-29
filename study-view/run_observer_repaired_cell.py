"""Selected runner with an isolated observer fix and existing coverage bridge."""
import sys
from pathlib import Path
import run_contextless_repair as coverage

original = coverage.original_invoke
def invoke(command, output, env=None, timeout=None):
    if len(command)>1 and Path(command[1]).name=='run_semweaver_treatment_case.py':
        command = [command[0], str(Path(__file__).with_name('repaired_treatment_entry.py')),
                   str(coverage.runner.SOURCE), *command[2:]]
    return original(command, output, env, timeout)

coverage.original_invoke = invoke
if __name__ == '__main__':
    coverage.runner.cell(Path(sys.argv[1]))
