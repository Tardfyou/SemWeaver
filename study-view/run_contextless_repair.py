"""Preserve the selected runner; add verified unsupported-observer fallback."""
import importlib.util
import json
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PROJECT = ROOT.parent
ORIGINAL = PROJECT / 'native-state-20260928/run_cell_v43.py'
spec = importlib.util.spec_from_file_location('selected_runner', ORIGINAL)
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
original_invoke = runner.invoke


def value(command, option):
    return command[command.index(option) + 1]


def invoke(command, output, env=None, timeout=None):
    code = original_invoke(command, output, env, timeout)
    if len(command) < 2 or Path(command[1]).name != 'run_semweaver_treatment_case.py' or not code:
        return code
    attempt = Path(value(command, '--output-dir'))
    probe = attempt / 'checker_execution_probe'
    result = runner.read(probe / 'RESULT.json') if (probe / 'RESULT.json').exists() else {}
    if result.get('error') != 'No supported CheckerContext-bearing method bodies':
        return code
    assert not (attempt / 'llm_exchanges.jsonl').exists(), 'Never replay consumed responses'
    assert not (attempt / 'RUN_MANIFEST.json').exists(), 'Unexpected model-phase checkpoint'
    failed = runner.read(probe / 'RUN_MANIFEST.json')
    assert failed['inputs_unchanged']
    case = Path(value(command, '--case-dir'))
    checker = (Path(value(command, '--starting-candidate')) if '--starting-candidate' in command
               else case / 'csa/SAGenTestChecker.cpp')
    proof = attempt / 'contextless-parity-probe'
    check = [sys.executable, str(ROOT / 'contextless_probe.py'), '--source-root', str(runner.SOURCE), '--',
             '--case-dir', str(case), '--checker', str(checker),
             '--linux-dir', value(command, '--evidence-dir'), '--output-dir', str(proof)]
    rc = original_invoke(check, output.with_name(output.name + '-contextless-parity'), env, 1800)
    assert rc == 0, 'Unsupported-context parity execution must be repaired before scoring'
    availability = runner.read(proof / 'CONTEXTLESS_COVERAGE.json')
    measurements = runner.read(proof / 'RESULT.json')
    manifest = runner.read(proof / 'RUN_MANIFEST.json')
    assert availability['status'] == 'verified_unavailable' and availability['diagnostic_copy_is_unmodified']
    assert measurements['execution_health'] == 'completed' and measurements['diagnostic_parity']
    assert manifest['inputs_unchanged'] and all(runner.sha(Path(p)) == h for p, h in manifest['inputs'].items())
    scans = measurements['scans']
    assert len(scans) == 4 and all(row['execution_valid'] for row in scans)
    assert {(r['side'], r['mode']) for r in scans} == {(s, m) for s in ('vulnerable', 'fixed') for m in ('original', 'traced')}
    assert all(row['trace_events'] == 0 and not row['trace_capped'] for row in scans)
    runner.save(attempt / 'CONTEXTLESS_FALLBACK.json', {
        'reason': 'observer_no_supported_context_sites', 'consumed_responses_before_fallback': 0,
        'failed_probe_manifest_sha256': runner.sha(probe / 'RUN_MANIFEST.json'),
        'parity_probe_manifest_sha256': runner.sha(proof / 'RUN_MANIFEST.json'),
        'availability_sha256': runner.sha(proof / 'CONTEXTLESS_COVERAGE.json'),
        'checker_sha256': runner.sha(checker), 'dynamic_evidence_available': False,
        'boundary': 'Fresh four-scan parity of byte-identical diagnostic copies; normal static native '
                    'records remain available, no dynamic observations fabricated. Original failure retained.'})
    fallback = dict(env or os.environ)
    fallback['SEMWEEVER_CHECKER_EXECUTION_TRACE'] = '0'
    return original_invoke(command, output.with_name(output.name + '-contextless-static'), fallback, timeout)


runner.invoke = invoke
if __name__ == '__main__':
    runner.cell(Path(sys.argv[1]))
