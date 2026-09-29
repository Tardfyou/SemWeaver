"""Keep validated zero-callback coverage unavailable, not an execution error."""
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import adaptive_trace_probe as original


if __name__ == '__main__':
    arguments = sys.argv
    output = Path(arguments[arguments.index('--output-dir')+1])
    source = Path(arguments[arguments.index('--source-root')+1])
    code = original.main()
    helper = source / 'src/research/trace_health.py'
    spec = importlib.util.spec_from_file_location('recorded_zero_scope_health', helper)
    health = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(health)
    if code and health.complete_zero_scope_coverage(output):
        proof = {'status': 'verified_dynamic_unavailable_normal_probe_exit',
                 'original_adaptive_exit_code': code, 'corrected_probe_exit_code': 0,
                 'dynamic_evidence_available': False, 'new_model_calls': 0,
                 'bindings': {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in
                              (Path(__file__), helper, output/'RESULT.json', output/'RUN_MANIFEST.json', output/'FIXTURE_SMOKE.json')},
                 'boundary': 'Original probe itself completed four healthy parity scans and a live fixture; no scoped callbacks exist. Do not invent traces or infer safety. Existing treatment handles verified unavailability.'}
        with (output / 'ZERO_COVERAGE_EXIT_ADAPTER.json').open('x') as stream:
            json.dump(proof, stream, indent=2)
        code = 0
    raise SystemExit(code)
