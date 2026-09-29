"""Architecture-correct diagnostic entry for an ARM64 patch, no model edits."""
import argparse
import hashlib
import importlib.util
import json
import re
import subprocess
import sys
from pathlib import Path


def architecture_command(command):
    if isinstance(command, str):
        if re.search(r'\bmake\b', command):
            return re.sub(r'\bARCH=x86\b', 'ARCH=arm64', command)
        return command
    if isinstance(command, (list, tuple)):
        result = ['ARCH=arm64' if str(word) == 'ARCH=x86' else word for word in command]
        if result and Path(str(result[0])).name == 'make' and not any(str(word).startswith('ARCH=') for word in result):
            result.append('ARCH=arm64')
        return result
    return command


def install(source):
    sys.path[:0] = [str(source), str(source / 'experiments/knighter/baseline/src'),
                   str(source / 'experiments/knighter/experiment')]
    from targets.linux import Linux
    from backends.csa import ClangBackend
    get_name = Linux.get_object_name
    checkout = Linux.checkout_commit
    scan = ClangBackend._run_checker_linux
    Linux.get_object_name = staticmethod(lambda file: Path(file).with_suffix('.o').as_posix()
                                       if str(file).startswith('arch/arm64/') else get_name(file))
    def checkout_arm(self, commit, *args, **kwargs):
        kwargs['arch'] = 'arm64'
        if kwargs.get('olddefcmd'):
            kwargs['olddefcmd'] = architecture_command(kwargs['olddefcmd'])
        return checkout(self, commit, *args, **kwargs)
    def scan_arm(self, *args, **kwargs):
        kwargs['arch'] = 'arm64'
        return scan(self, *args, **kwargs)
    Linux.checkout_commit = checkout_arm
    ClangBackend._run_checker_linux = scan_arm
    run, popen = subprocess.run, subprocess.Popen
    def run_arm(command, *args, **kwargs):
        return run(architecture_command(command), *args, **kwargs)
    def popen_arm(command, *args, **kwargs):
        if (isinstance(command, (list, tuple)) and len(command) > 1
                and Path(str(command[1])).name == 'run_checker_trace_probe.py'):
            command = [command[0], str(Path(__file__).resolve()), '--source-root', str(source),
                       '--script', 'run_checker_trace_probe.py', '--', *command[2:]]
        return popen(architecture_command(command), *args, **kwargs)
    subprocess.run, subprocess.Popen = run_arm, popen_arm


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source-root', required=True, type=Path)
    parser.add_argument('--script', required=True, choices=('validate_frozen_csa_candidate.py', 'collect_frozen_csa_evidence.py', 'run_checker_trace_probe.py', 'run_semweaver_treatment_case.py'))
    parser.add_argument('arguments', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    install(args.source_root)
    script = args.source_root / 'experiments/knighter/experiment' / args.script
    spec = importlib.util.spec_from_file_location('arm64_original_entry', script)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    arguments = args.arguments[1:] if args.arguments[:1] == ['--'] else args.arguments
    sys.argv = [str(script), *arguments]
    result = module.main()
    output_option = '--output-dir'
    if output_option in arguments:
        output = Path(arguments[arguments.index(output_option) + 1])
        calls = 0
        if args.script == 'run_semweaver_treatment_case.py':
            receipt = output / 'RUN_MANIFEST.json'
            calls = json.loads(receipt.read_text()).get('model_calls_used') if receipt.exists() else None
        record = {'architecture': 'arm64', 'mapping': 'exact arch/arm64 source-relative object',
                  'entry_sha256': hashlib.sha256(script.read_bytes()).hexdigest(),
                  'bridge_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  'return_code': result, 'new_llm_calls': calls,
                  'boundary': 'Architecture/build adapter repair only; checker bytes are unchanged.'}
        with (output / 'ARCHITECTURE_ADAPTER.json').open('x') as handle:
            json.dump(record, handle, indent=2)
            handle.write('\n')
    return result


if __name__ == '__main__':
    raise SystemExit(main())
