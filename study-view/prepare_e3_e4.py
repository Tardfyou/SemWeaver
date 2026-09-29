"""Preserve the prior outcome-independent repeated cohort for the new method."""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PROJECT = ROOT.parent
SUMMARY = PROJECT / 'LLM-Native/artifacts/fse_revision/scanfix_v26_corrected_summary_v1/RESULT.json'


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


if __name__ == '__main__':
    old = json.loads(SUMMARY.read_text())
    repetitions = old['repeated12']
    cohorts = [{row['case_id'] for row in run['rows']} for name in repetitions for run in repetitions[name]]
    assert cohorts and all(cohort == cohorts[0] for cohort in cohorts)
    assert len(cohorts[0]) == 12
    chosen = json.loads((ROOT / 'SELECTION.json').read_text())
    all_cases = json.loads((ROOT / 'MATRIX_FREEZE.json').read_text())['subjects']
    plan = {'status': 'prepared_not_executed', 'source_revision': chosen['source_revision'],
            'sampling_frame': all_cases, 'repeated_samples': sorted(cohorts[0]),
            'old_selection_summary_sha256': sha(SUMMARY), 'generator_sha256': sha(__file__),
            'E3': {'whole_cohort_comparison': 'current39 native versus no_internal at32responsecap',
                   'repeated_samples': 12, 'repeats_per_condition': 3,
                   'model': 'gpt-6-luna', 'reasoning_effort': 'high', 'response_cap': 32,
                   'reuse_first_decode': 'Only matching selected-method executions from the main cohort; verify exact inputs/protocol first.'},
            'E4': {'models': ['gpt-6-luna', 'glm-5.3', 'glm-5.3-flash'],
                   'luna_reasoning_effort': 'high', 'whole_cohort_samples_per_model': 39,
                   'repeated_samples': 12, 'repeats_per_model': 3,
                   'response_cap': 2, 'output_token_ceiling': 16384,
                   'conditions': ['native'], 'driver_and_provider_smokes': 'pending'},
            'boundary': 'No sample is replaced because it wins or loses. Repeated decodes are not '
                        'new independent bugs. Older source-version outcomes are preserved but cannot '
                        'be relabeled as the selected method. ARM64-correct G04 and verified observer '
                        'availability handling apply wherever relevant. This file launches no calls.'}
    with (ROOT / 'E3_E4_PREPARATION.json').open('x') as handle:
        json.dump(plan, handle, indent=2)
        handle.write('\n')
    print(json.dumps({'status': plan['status'], 'repeated_samples': plan['repeated_samples'],
                      'E4_models': plan['E4']['models'], 'new_model_calls': 0}))
