"""Same one-response/paired-feedback protocol, explicit model and budget profile."""
import hashlib
import importlib.util
import json
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PROJECT = ROOT.parent
SOURCE = PROJECT / 'LLM-Native/SemWeaver-v43'
DATA = PROJECT / 'LLM-Native/artifacts/fse_revision'
sys.path.insert(0, str(SOURCE / 'experiments/knighter/experiment'))
from run_semweaver_full_treatment_batch import provider_interruption_events


def read(path): return json.loads(Path(path).read_text())
def sha(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def save(path, value, exclusive=True):
    with Path(path).open('x' if exclusive else 'w') as handle:
        json.dump(value, handle, indent=2)
        handle.write('\n')


def stop_reason(state, cap):
    if state.get('retained_pds'): return 'retained_positive_zero_fixed_saturation'
    if state.get('guard_rejected_pds'): return 'raw_saturation_rejected_requires_quality_diagnosis'
    if state['stagnation'] >= 3: return 'three_validations_without_retained_improvement'
    if state['invalid_streak'] >= 3: return 'three_model_attempts_without_valid_candidate'
    if state['calls'] >= cap: return 'model_call_ceiling'
    return None


def environment(profile, root, arm):
    key = Path('/run/private/model-key')
    assert key.stat().st_mode & 0o077 == 0
    result = os.environ.copy()
    for name in ('OPENAI_API_KEY', 'ANTHROPIC_AUTH_TOKEN', 'ANTHROPIC_API_KEY',
                 'SEMWEEVER_PIN_NATIVE_WITNESS', 'SEMWEEVER_NATIVE_MISMATCH_FACTS',
                 'SEMWEEVER_CHECKER_EXECUTION_TRACE', 'SEMWEEVER_NATIVE_WITNESS_ROOT'):
        result.pop(name, None)
    result.update(SEMWEEVER_MODEL=profile['model'], SEMWEEVER_REVIEW_MODEL=profile['model'],
                  SEMWEEVER_LLM_PROVIDER=profile['provider'], SEMWEEVER_WIRE_API=profile['wire_api'],
                  SEMWEEVER_REASONING_EFFORT=profile['reasoning_effort'], SEMWEEVER_LLM_TIMEOUT='1800',
                  SEMWEEVER_RATE_LIMIT_MAX_ATTEMPTS='8', SEMWEEVER_PILOT_BUDGET_FILE=str(root / 'BUDGET.json'))
    if profile['provider'] == 'anthropic':
        result.update(ANTHROPIC_AUTH_TOKEN=key.read_text().strip(), ANTHROPIC_BASE_URL=profile['base_url'])
    else:
        result.update(OPENAI_API_KEY=key.read_text().strip(), OPENAI_BASE_URL=profile['base_url'])
    if arm == 'native':
        result.update(SEMWEEVER_PIN_NATIVE_WITNESS='1', SEMWEEVER_NATIVE_MISMATCH_FACTS='1',
                      SEMWEEVER_CHECKER_EXECUTION_TRACE='1', SEMWEEVER_NATIVE_WITNESS_ROOT=str(DATA))
    return result


def invoke(command, output, env=None, timeout=None):
    with output.with_suffix('.stdout').open('x') as stdout, output.with_suffix('.stderr').open('x') as stderr:
        return subprocess.run(command, stdout=stdout, stderr=stderr, env=env, timeout=timeout).returncode


def cell(config_path):
    cfg = read(config_path)
    root = Path(config_path).parent
    plan = read(root / 'RUN_PLAN.json')
    assert all(sha(path) == value for path, value in plan['inputs'].items())
    assert not cfg.get('prefix_attempts'), 'Profile first runs do not silently reuse unmatched prefixes'
    case, arm, profile = cfg['case_id'], cfg['arm'], cfg['profile']
    cap = plan['model_response_cap']
    original = DATA / 'generate_only_materialized_v1/cases' / case / 'csa/SAGenTestChecker.cpp'
    start_path = Path(cfg.get('starting_result', PROJECT / 'native-trace-20260928/starting' / case / 'RESULT.json'))
    start = read(start_path)
    assert start['execution_valid'] and start['candidate_sha256'] == sha(original)
    quality_cache = {}
    for name in cfg.get('prior_adoptions', []):
        for row in read(name)['pool']:
            assert sha(row['candidate']) == row['candidate_sha256']
            assert sha(row['quality_result']) == row['quality_result_sha256']
            quality_cache[row['candidate_sha256']] = row['unsafe_target_hits']
    def quality(candidate):
        digest = sha(candidate)
        if digest not in quality_cache:
            target = root / 'quality' / digest
            target.parent.mkdir(exist_ok=True)
            rc = invoke([sys.executable, str(SOURCE / 'experiments/knighter/experiment/run_array_guard_sanity.py'),
                         '--candidate', str(candidate), '--output-dir', str(target)], root / ('quality-' + digest))
            assert rc == 0, 'Quality execution error, unscored'
            result = read(target / 'RESULT.json')
            assert result['status'] == 'completed' and result['candidate_sha256'] == digest
            quality_cache[digest] = result['unsafe_target_hits']
        return set(quality_cache[digest])
    required = quality(original) if arm == 'native' else set()
    best = {key: start[key] for key in ('vulnerable_alerts', 'fixed_alerts')}
    best.update(candidate=str(original), candidate_sha256=sha(original), origin=str(start_path), result_sha256=sha(start_path))
    state = {'calls': 0, 'stagnation': 0, 'invalid_streak': 0, 'retained_pds': best['vulnerable_alerts'] > 0 and best['fixed_alerts'] == 0}
    rows, latest = [], None
    save(root / 'INITIAL_STATE.json', {'state': state, 'best': best})
    env = environment(profile, root, arm)
    # Reuse the verified observer-availability handler without modifying its source.
    spec = importlib.util.spec_from_file_location('profile_coverage_handler', ROOT / 'run_contextless_repair.py')
    coverage = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(coverage)
    while not stop_reason(state, cap):
        assert time.time() < plan['deadline_epoch'], 'Operational deadline, partial'
        attempt = root / f'continuation-{len(rows)+1:02d}'
        command = [sys.executable, str(SOURCE / 'experiments/knighter/experiment/run_semweaver_treatment_case.py'),
                   '--config', str(SOURCE / 'config/config.yaml'), '--case-dir', str(DATA / 'generate_only_materialized_v1/cases' / case),
                   '--evidence-bundle', cfg['evidence_bundle'], '--evidence-dir', cfg['linux_dir'],
                   '--output-dir', str(attempt), '--max-iterations', '1', '--max-model-calls', '1',
                   '--max-tokens', str(plan['output_token_ceiling']), '--evidence-mode', arm, '--objective', cfg['objective']]
        if latest:
            command += ['--starting-candidate', str(latest / 'SAGenTestChecker.cpp')]
            feedback = latest / 'frozen_validation/RESULT.json'
            command += ['--feedback-result', str(feedback)] if feedback.exists() else ['--feedback-run-manifest', str(latest / 'RUN_MANIFEST.json')]
        elif cfg['objective'] == 'target_hit_recovery':
            command += ['--feedback-result', str(start_path)]
        if cfg['objective'] == 'precision_refine':
            command += ['--fixed-reports-dir', cfg['fixed_reports_dir']]
        if cfg.get('architecture') == 'arm64':
            command = [command[0], str(ROOT / 'arm64_entry.py'), '--source-root', str(SOURCE),
                       '--script', 'run_semweaver_treatment_case.py', '--', *command[2:]]
            invoke(command, root / attempt.name, env)
        else:
            coverage.invoke(command, root / attempt.name, env)
        for retry in range(8):
            receipt = attempt / 'RUN_MANIFEST.json'
            events = provider_interruption_events(attempt / 'run_events.jsonl')
            metadata = read(receipt) if receipt.exists() else {}
            if receipt.exists() and not events:
                break
            error = str(metadata.get('error_message', ''))
            retryable = any(token in error for token in ('Error code: 502', 'Error code: 503', 'Error code: 504', 'Connection error.'))
            if not retryable or metadata.get('model_calls_used', 0) or (attempt / 'llm_exchanges.jsonl').exists():
                raise RuntimeError('Interrupted profile needs checkpoint repair; consumed responses are never replayed')
            save(root / (attempt.name + '-TRANSPORT.json'), {'status': 'unscored_transport_interruption',
                 'consumed_model_responses': 0, 'retry_number': retry+1, 'attempt': str(attempt)})
            if retry == 7:
                raise RuntimeError('Transport retry ceiling, partial cell retained')
            time.sleep(min(60, 10 * 2**retry))
            attempt = root / f'continuation-{len(rows)+1:02d}-transport-r{retry+1}'
            command[command.index('--output-dir') + 1] = str(attempt)
            if cfg.get('architecture') == 'arm64':
                invoke(command, root / attempt.name, env)
            else:
                coverage.invoke(command, root / attempt.name, env)
        if provider_interruption_events(attempt / 'run_events.jsonl') or not (attempt / 'RUN_MANIFEST.json').exists():
            raise RuntimeError('Provider/harness interruption needs verified checkpoint repair')
        manifest = read(attempt / 'RUN_MANIFEST.json')
        assert manifest['model'] == profile['model'] and manifest['reasoning_effort'] == profile['reasoning_effort']
        assert manifest['temperature'] == 0 and manifest['model_calls_used'] == 1
        candidate = attempt / 'SAGenTestChecker.cpp'
        assert sha(candidate) == manifest['candidate_sha256']
        state['calls'] += 1
        validation = None
        if manifest['success']:
            command = [sys.executable, str(SOURCE / 'experiments/knighter/experiment/validate_frozen_csa_candidate.py'),
                       '--candidate', str(candidate), '--case-dir', str(DATA / 'generate_only_materialized_v1/cases' / case),
                       '--linux-dir', cfg['linux_dir'], '--output-dir', str(attempt / 'frozen_validation'),
                       '--backend-workspace', str(attempt / 'validation_backend'), '--jobs', '4']
            if cfg.get('architecture') == 'arm64':
                command = [command[0], str(ROOT / 'arm64_entry.py'), '--source-root', str(SOURCE),
                           '--script', 'validate_frozen_csa_candidate.py', '--', *command[2:]]
            invoke(command, root / (attempt.name + '-validation'))
            validation = read(attempt / 'frozen_validation/RESULT.json')
            assert validation['candidate_sha256'] == sha(candidate) and validation['execution_valid']
            assert not validation.get('infrastructure_errors') and not validation.get('scan_failure_artifacts')
        valid, accepted, lost = bool(validation), False, []
        if valid and validation['vulnerable_alerts'] > 0 and (best['vulnerable_alerts'] == 0 or validation['fixed_alerts'] < best['fixed_alerts']):
            hits = quality(candidate) if arm == 'native' else set()
            lost = sorted(required - hits)
            if not lost:
                best = {key: validation[key] for key in ('vulnerable_alerts','fixed_alerts')}
                best.update(candidate=str(candidate), candidate_sha256=sha(candidate),
                            origin=str(attempt / 'frozen_validation/RESULT.json'), result_sha256=sha(attempt / 'frozen_validation/RESULT.json'))
                required, accepted = hits, True
        if accepted: state.update(stagnation=0, invalid_streak=0)
        elif valid: state.update(stagnation=state['stagnation']+1, invalid_streak=0)
        else: state['invalid_streak'] += 1
        state['retained_pds'] = best['vulnerable_alerts'] > 0 and best['fixed_alerts'] == 0
        state['guard_rejected_pds'] = bool(lost and validation and validation.get('pds'))
        rows.append({'attempt': str(attempt), 'manifest_sha256': sha(attempt / 'RUN_MANIFEST.json'),
                     'calls': 1, 'paired_valid': valid, 'adopted_improvement': accepted, 'lost_controlled_hits': lost,
                     'vulnerable_alerts': validation['vulnerable_alerts'] if valid else None,
                     'fixed_alerts': validation['fixed_alerts'] if valid else None, 'state_after': dict(state)})
        latest = attempt
        save(root / 'PROGRESS.json', {'state': state, 'best': best, 'rows': rows}, False)
    assert state['calls'] <= cap
    save(root / 'RESULT.json', {'status': 'completed', 'case_id': case, 'arm': arm, 'profile': profile,
         'state': state, 'stop_reason': stop_reason(state, cap), 'selected': best, 'rows': rows,
         'boundary': 'Actual model identity preserved; same selected-method cadence and adoption, profile/budget explicit.'})


if __name__ == '__main__':
    cell(Path(sys.argv[1]))
