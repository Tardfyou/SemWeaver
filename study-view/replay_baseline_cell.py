"""Independently replay KNighter's actual outer-attempt output checkers."""
import hashlib
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PROJECT = ROOT.parent
DATA = PROJECT / 'LLM-Native/artifacts/fse_revision'
SOURCE = PROJECT / 'LLM-Native/SemWeaver-v43'
LANE = PROJECT / 'LLM-Native/SemWeaver/artifacts/external/linux-knighter-replay-20260928'


def read(path):
    return json.loads(path.read_text())


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    cell = Path(sys.argv[1]).resolve()
    execution = read(cell / 'EXECUTION_MANIFEST.json')
    assert execution['status'] == 'completed_baseline_pending_paired_replay'
    plan = read(cell / 'RUN_PLAN.json')
    assert all(sha(path) == value for path, value in plan['inputs'].items())
    health = read(cell / 'exchanges.health.json')
    assert health['status'] == 'completed' and not health['errors']
    summaries = list((cell / 'output').glob('*/MATCHED_BASELINE_RESULT.json'))
    assert len(summaries) == 1
    summary = read(summaries[0])
    assert summary['execution_valid'] and summary['model_calls_used'] == health['consumed_model_responses'] <= 32
    case = execution['case_id']
    original = DATA / 'generate_only_materialized_v1/cases' / case / 'csa/SAGenTestChecker.cpp'
    start = next(r for r in read(DATA / 'scanfix_v26_corrected_summary_v1/RESULT.json')['starting']['rows'] if r['case_id'] == case)
    assert sha(original) == start['candidate_sha256']
    selected = {'candidate': str(original), 'candidate_sha256': sha(original),
                'vulnerable_alerts': start['vulnerable_alerts'], 'fixed_alerts': start['fixed_alerts'],
                'origin': str(DATA / start['validation_path']), 'result_sha256': start['validation_sha256']}
    rows = []
    seen = {sha(original)}
    for attempt in summary['results']:
        candidate = summaries[0].parent / 'refinements' / f"attempt_{attempt['attempt_id']}.cpp"
        if not candidate.exists():
            assert not attempt['refined'], 'Refined output missing'
            continue
        digest = sha(candidate)
        if digest in seen:
            continue
        seen.add(digest)
        destination = cell / 'paired-replay' / f"attempt-{attempt['attempt_id']}"
        command = [sys.executable, str(SOURCE / 'experiments/knighter/experiment/validate_frozen_csa_candidate.py'),
                   '--candidate', str(candidate), '--case-dir', str(DATA / 'generate_only_materialized_v1/cases' / case),
                   '--linux-dir', str(LANE), '--output-dir', str(destination),
                   '--backend-workspace', str(cell / 'replay-backend' / f"attempt-{attempt['attempt_id']}"), '--jobs', '4']
        with (cell / f"replay-{attempt['attempt_id']}.stdout").open('x') as stdout, (cell / f"replay-{attempt['attempt_id']}.stderr").open('x') as stderr:
            subprocess.run(command, stdout=stdout, stderr=stderr, timeout=1800)
        result = read(destination / 'RESULT.json')
        assert result['candidate_sha256'] == digest and result['execution_valid']
        assert not result.get('infrastructure_errors') and not result.get('scan_failure_artifacts')
        improved = result['vulnerable_alerts'] > 0 and (selected['vulnerable_alerts'] == 0 or result['fixed_alerts'] < selected['fixed_alerts'])
        if improved:
            selected = {'candidate': str(candidate), 'candidate_sha256': digest,
                        'vulnerable_alerts': result['vulnerable_alerts'], 'fixed_alerts': result['fixed_alerts'],
                        'origin': str(destination / 'RESULT.json'), 'result_sha256': sha(destination / 'RESULT.json')}
        rows.append({'attempt_id': attempt['attempt_id'], 'upstream_refined': attempt['refined'],
                     'candidate_sha256': digest, 'result_sha256': sha(destination / 'RESULT.json'),
                     'vulnerable_alerts': result['vulnerable_alerts'], 'fixed_alerts': result['fixed_alerts'],
                     'common_adoption': improved})
    assert all(sha(path) == value for path, value in plan['inputs'].items())
    output = {'status': 'completed', 'case_id': case, 'selected': selected, 'rows': rows,
              'model_responses': summary['model_calls_used'], 'new_model_calls': 0,
              'baseline_summary_sha256': sha(summaries[0]), 'exchange_health_sha256': sha(cell / 'exchanges.health.json'),
              'replay_script_sha256': sha(__file__),
              'boundary': 'Actual published outer-attempt outputs, not arbitrary intermediate edits. '
                          'The same partial-improvement retention rule is used; PDS is secondary. '
                          'Repeated output hashes are not replayed or counted twice.'}
    with (cell / 'PAIRED_RESULT.json').open('x') as handle:
        json.dump(output, handle, indent=2)
        handle.write('\n')
    print(json.dumps({'case_id': case, 'model_responses': summary['model_calls_used'],
                      'paired_outcome': [selected['vulnerable_alerts'], selected['fixed_alerts']]}))


if __name__ == '__main__':
    main()
