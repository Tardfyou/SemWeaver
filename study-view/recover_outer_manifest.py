"""Recover only an empty launcher receipt from a verified completed cell."""
import hashlib
import json
import sys
import time
from pathlib import Path


def read(path):
    return json.loads(path.read_text())


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    cell = Path(sys.argv[1]).resolve()
    receipt = cell / 'RUN_MANIFEST.json'
    assert receipt.exists() and receipt.stat().st_size == 0
    plan = read(cell / 'RUN_PLAN.json')
    result = read(cell / 'RESULT.json')
    assert result['status'] == 'completed'
    assert all(sha(path) == digest for path, digest in plan['inputs'].items())
    calls = 0
    for row in result['rows']:
        attempt = Path(row['attempt'])
        manifest = read(attempt / 'RUN_MANIFEST.json')
        assert sha(attempt / 'RUN_MANIFEST.json') == row['manifest_sha256']
        assert sha(attempt / 'SAGenTestChecker.cpp') == manifest['candidate_sha256']
        calls += manifest['model_calls_used']
        if row['paired_valid']:
            validation = read(attempt / 'frozen_validation/RESULT.json')
            assert validation['execution_valid'] and not validation.get('infrastructure_errors')
            assert validation['candidate_sha256'] == manifest['candidate_sha256']
            assert all(validation[key] == row[key] for key in ('vulnerable_alerts', 'fixed_alerts'))
    assert calls == result['state']['calls'] <= plan['model_response_cap']
    selected = result['selected']
    assert sha(selected['candidate']) == selected['candidate_sha256']
    if selected.get('result_sha256'):
        assert sha(selected['origin']) == selected['result_sha256']
    else:
        assert selected['origin'] == 'starting_checker'
        project = Path(__file__).resolve().parent.parent
        start_path = project / 'LLM-Native/artifacts/fse_revision/scanfix_v26_corrected_summary_v1/RESULT.json'
        start = next(r for r in read(start_path)['starting']['rows'] if r['case_id'] == result['case_id'])
        assert start['candidate_sha256'] == selected['candidate_sha256']
        assert all(start[key] == selected[key] for key in ('vulnerable_alerts', 'fixed_alerts'))
    metadata = {**plan, 'status': 'completed', 'return_code': None,
                'inputs_unchanged': True, 'result_sha256': sha(cell / 'RESULT.json'),
                'receipt_recovered_at': time.time(),
                'receipt_recovery': 'Launcher receipt was empty after disk exhaustion. Actual completed '
                                    'result, all attempt ledgers, counts and input/output hashes verified. '
                                    'The lost launcher exit-code observation is not invented.',
                'recovery_script_sha256': sha(__file__)}
    with receipt.open('w') as handle:
        json.dump(metadata, handle, indent=2)
        handle.write('\n')
    print(json.dumps({'cell': cell.name, 'verified_model_responses': calls,
                      'receipt_recovered': True, 'exit_code_observation_available': False}))


if __name__ == '__main__':
    main()
