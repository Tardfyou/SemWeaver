"""Bind the fresh ARM evidence raw files to their actual frozen witness root."""
import importlib.util
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('arm64_refine_binding', ROOT / 'run_arm64_refine.py')
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)
invoke = bridge.runner.invoke


def bound_invoke(command, output, env=None, timeout=None):
    if env is not None:
        env = dict(env)
        env['SEMWEEVER_NATIVE_WITNESS_ROOT'] = str(ROOT)
    return invoke(command, output, env, timeout)


bridge.runner.invoke = bound_invoke
if __name__ == '__main__':
    bridge.runner.cell(Path(sys.argv[1]))
