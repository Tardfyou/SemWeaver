"""Offline verification of the two complete single-call natural baseline stops."""
import hashlib
import importlib.util
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PROJECT = ROOT.parent
SOURCE = PROJECT / 'LLM-Native/SemWeaver-v43'
DATA = PROJECT / 'LLM-Native/artifacts/fse_revision'


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    script = SOURCE / 'experiments/knighter/experiment/run_matched_knighter.py'
    spec = importlib.util.spec_from_file_location('baseline_prompt_audit', script)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    from agent import prompt_template_dir
    batch_path = DATA / 'generate_only_knighter_gpt6luna_high_v8/BATCH_RESULT.json'
    batch = json.loads(batch_path.read_text())
    template_path = prompt_template_dir / 'check_report.md'
    rows = []
    for item in batch['cases']:
        case = item['case_id']
        if not case.startswith(('G21_', 'G39_')):
            continue
        result = item['result']
        assert result['execution_valid'] and result['model_calls_used'] == 1
        assert not result['model_call_budget_exhausted']
        assert result['max_tries'] == 2 and result['max_fp_reports'] == 5
        assert all(not attempt['refined'] and attempt['result'] == 'No-FP' for attempt in result['results'])
        case_dir = DATA / 'generate_only_materialized_v1/cases' / case
        assert sha(case_dir / 'csa/SAGenTestChecker.cpp') == result['checker_sha256']
        assert sha(case_dir / 'patches/knighter_patch.md') == result['patch_sha256']
        reports = module.fixed_reports(PROJECT / 'native-trace-20260928/fixed_reports' / case / 'fixed', 5)
        assert len(reports) == 1 and reports[0]['id'] == result['fixed_report_ids'][0]
        prompt = template_path.read_text().replace('{{input_bug_report}}', reports[0]['content'])
        prompt = prompt.replace('{{input_bug_pattern}}', (case_dir / 'metadata/pattern.txt').read_text().strip('```'))
        prompt = prompt.replace('{{input_patch}}', (case_dir / 'patches/knighter_patch.md').read_text())
        history = batch_path.parent / result['checker_id'] / 'prompt_history/0'
        old_prompt = history / ('check_report-' + reports[0]['id'] + '.md')
        response = history / ('response_check_report-' + reports[0]['id'] + '.md')
        exact = prompt == old_prompt.read_text()
        rows.append({'case_id': case, 'exact_prompt_equivalence': exact,
                     'reuse_eligible': exact and response.is_file() and 'SKIP' not in response.read_text(),
                     'prompt_sha256': sha(old_prompt), 'response_sha256': sha(response),
                     'starting_checker_sha256': result['checker_sha256'],
                     'recorded_response_ceiling': result['max_model_calls'], 'recorded_responses': 1,
                     'proposed_response_ceiling': 32,
                     'termination': 'Actual algorithm ended No-FP before either ceiling; no refinement attempted.'})
    assert len(rows) == 2
    payload = {'status': 'offline_natural_stop_reuse_audit', 'rows': rows,
               'baseline_batch_sha256': sha(batch_path), 'template_sha256': sha(template_path),
               'auditor_sha256': sha(__file__), 'new_model_calls': 0,
               'boundary': 'Only complete naturally ended single-call histories can qualify here. '
                           'The old eight-call record is retained, not relabeled as a32-call execution. '
                           'The larger capacity cannot affect this prior natural stop; request bytes, '
                           'model settings and inputs remain explicit.'}
    with (ROOT / 'BASELINE_NATURAL_REUSE_AUDIT.json').open('x') as handle:
        json.dump(payload, handle, indent=2)
        handle.write('\n')
    print(json.dumps({'new_model_calls': 0, 'rows': rows}))


if __name__ == '__main__':
    main()
