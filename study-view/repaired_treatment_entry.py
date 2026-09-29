"""Route trace-probe subprocesses through the isolated observer bug fix."""
import importlib.util
import sys
from pathlib import Path


def main():
    source = Path(sys.argv[1])
    arguments = sys.argv[2:]
    sys.path.insert(0, str(source))
    import subprocess
    original = subprocess.Popen
    def popen(command, *args, **kwargs):
        if isinstance(command, list) and len(command)>1 and Path(command[1]).name=='run_checker_trace_probe.py':
            command = [command[0], str(Path(__file__).with_name('fixed_trace_probe.py')),
                       '--source-root', str(source), '--', *command[2:]]
        return original(command, *args, **kwargs)
    subprocess.Popen = popen
    script = source/'experiments/knighter/experiment/run_semweaver_treatment_case.py'
    spec = importlib.util.spec_from_file_location('observer_repaired_treatment', script)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    sys.argv = [str(script), *arguments]
    return module.main()


if __name__ == '__main__':
    raise SystemExit(main())
