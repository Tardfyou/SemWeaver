"""Verify diagnostic parity when the observer has no supported context sites.

The diagnostic copy is byte-identical and emits no invented dynamic observations.
The normal v43 probe still compiles both copies and executes all four scans.
"""
import argparse
import hashlib
import importlib.util
import json
import sys
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source-root', required=True, type=Path)
    parser.add_argument('probe_arguments', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    script = args.source_root / 'experiments/knighter/experiment/run_checker_trace_probe.py'
    spec = importlib.util.spec_from_file_location('contextless_original_probe', script)
    probe = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(probe)
    original_instrument = probe.instrument
    unavailable = []
    def instrument(code, roots, support):
        try:
            return original_instrument(code, roots, support)
        except ValueError as error:
            if str(error) != 'No supported CheckerContext-bearing method bodies':
                raise
            unavailable.append(hashlib.sha256(code).hexdigest())
            return code, {'observer_status': 'unavailable_no_supported_context', 'sites': [],
                          'diagnostic_copy_is_unmodified': True}
    probe.instrument = instrument
    arguments = args.probe_arguments
    if arguments[:1] == ['--']:
        arguments = arguments[1:]
    sys.argv = [str(script), *arguments]
    code = probe.main()
    output = Path(arguments[arguments.index('--output-dir') + 1])
    result = json.loads((output / 'RESULT.json').read_text())
    manifest = json.loads((output / 'RUN_MANIFEST.json').read_text())
    clone = output / 'InstrumentedChecker.cpp'
    same = bool(unavailable and clone.exists() and hashlib.sha256(clone.read_bytes()).hexdigest() == unavailable[0])
    payload = {'status': 'verified_unavailable' if code == 0 and same else 'unscored',
               'observer_status': 'unavailable_no_supported_context',
               'dynamic_evidence_available': False, 'diagnostic_copy_is_unmodified': same,
               'starting_checker_sha256': unavailable[0] if unavailable else None,
               'diagnostic_parity': result.get('diagnostic_parity'),
               'inputs_unchanged': manifest.get('inputs_unchanged'),
               'wrapper_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
               'original_probe_sha256': hashlib.sha256(script.read_bytes()).hexdigest(),
               'boundary': 'The mode named traced in the reused scan layout is an explicitly '
                           'unmodified diagnostic copy here. No native state, callback coverage '
                           'or program safety is inferred from missing dynamic records.'}
    with (output / 'CONTEXTLESS_COVERAGE.json').open('x') as handle:
        json.dump(payload, handle, indent=2)
        handle.write('\n')
    return 0 if payload['status'] == 'verified_unavailable' else 1


if __name__ == '__main__':
    raise SystemExit(main())
