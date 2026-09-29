"""Repair observer-only statement name capture; never edit scored checkers."""
import argparse
import hashlib
import importlib.util
import json
import sys
import types
from pathlib import Path


def repaired_instrumenter(source_root):
    path = source_root / 'src/research/checker_trace.py'
    original = path.read_text()
    old = '''            if context:
                scope=f'semweaver_trace::ContextScope __semweaver_scope_{begin}({context}, {stmt});\\n'.encode()
                edits.append((begin+1, begin+1, b"\\n"+scope+marker(begin, "enter")+b"\\n"))'''
    new = '''            if context:
                statement_expression = stmt
                stmt = f'__semweaver_statement_{begin}'
                capture = f'const clang::Stmt *{stmt} = {statement_expression};\\n'.encode()
                scope=f'semweaver_trace::ContextScope __semweaver_scope_{begin}({context}, {stmt});\\n'.encode()
                edits.append((begin+1, begin+1, b"\\n"+capture+scope+marker(begin, "enter")+b"\\n"))'''
    assert original.count(old) == 1, 'Observer source drift; do not guess a patch'
    module = types.ModuleType('statement_capture_repaired_observer')
    exec(compile(original.replace(old, new), str(path)+'[statement-capture-repair]', 'exec'), module.__dict__)
    return module.instrument, hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source-root', type=Path, required=True)
    parser.add_argument('arguments', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    script = args.source_root/'experiments/knighter/experiment/run_checker_trace_probe.py'
    sys.path.insert(0, str(args.source_root))
    spec = importlib.util.spec_from_file_location('repaired_original_probe', script)
    probe = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(probe)
    probe.instrument, original_sha = repaired_instrumenter(args.source_root)
    arguments = args.arguments[1:] if args.arguments[:1] == ['--'] else args.arguments
    sys.argv = [str(script), *arguments]
    code = probe.main()
    output = Path(arguments[arguments.index('--output-dir')+1])
    receipt = {'repair': 'Capture callback statement before any local parameter-name shadowing',
               'original_instrumenter_sha256': original_sha,
               'repair_wrapper_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
               'scored_checker_modified': False, 'probe_return_code': code,
               'boundary': 'Only disposable observer copies changed; original four-scan parity and coverage gates remain mandatory.'}
    with (output/'OBSERVER_REPAIR.json').open('x') as handle:
        json.dump(receipt, handle, indent=2)
    return code


if __name__ == '__main__':
    raise SystemExit(main())
