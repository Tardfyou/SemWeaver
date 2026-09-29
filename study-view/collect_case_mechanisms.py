"""Trace all main comparative differences to code/attempts, without new scans.

Diffs describe emitted code. They do not prove that an evidence record caused
an edit or that warnings correspond to the patch's target vulnerability.
"""
import difflib
import hashlib
import json
from pathlib import Path

from verify_final_artifact import Auditor, direction

ROOT = Path(__file__).resolve().parent


def read(path):
    return json.loads(Path(path).read_text())


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


if __name__ == '__main__':
    auditor = Auditor(ROOT, ROOT.parent)
    main = auditor.main()
    starts = {r['case_id']: r for r in main['starting']['rows']}
    arms = {arm: {r['case_id']: r for r in main['portfolios'][arm]['rows']}
            for arm in ('native', 'no_internal')}
    repeated = read(ROOT / 'REPEATED_ABLATION_STATUS.json')
    repeats = {r['case_id']: r for r in repeated['rows']}
    rows = []
    for case, start in starts.items():
        chosen = {arm: arms[arm][case] for arm in arms}
        outcome = direction([chosen['native'][k] for k in ('vulnerable_alerts', 'fixed_alerts')],
                            [chosen['no_internal'][k] for k in ('vulnerable_alerts', 'fixed_alerts')])
        if outcome == 'tie_or_other':
            continue
        before = Path(start['candidate']).read_text().splitlines()
        record = {'case_id': case, 'raw_direction_key': outcome,
                  'comparison_label': 'native_comparative_benefit' if outcome.startswith('positive') else 'native_comparative_disadvantage',
                  'starting': start, 'arms': {}, 'repeated_comparison': repeats.get(case)}
        for arm, selected in chosen.items():
            cell = Path(selected['cell'])
            result = read(cell / 'RESULT.json')
            attempts = []
            for attempt in result['rows']:
                path = Path(attempt['attempt'])
                manifest = read(path / 'RUN_MANIFEST.json')
                attempts.append({**attempt, 'candidate_sha256': manifest['candidate_sha256'],
                                 'probe_availability': manifest.get('checker_execution_probe_status'),
                                 'preloaded_internal_evidence_ids': manifest.get('preloaded_internal_evidence_ids', []),
                                 'failure_type': manifest.get('failure_type'),
                                 'latest_failure_title': manifest.get('latest_failure_title')})
            v, f = selected['vulnerable_alerts'], selected['fixed_alerts']
            sv, sf = start['vulnerable_alerts'], start['fixed_alerts']
            own = ('recovered_version_warning' if sv == 0 and v > 0 else
                   'retained_warning_reduced_fixed_reports' if sv > 0 and v > 0 and f < sf else
                   'unchanged_version_warning_counts' if (sv, sf) == (v, f) else 'other_recorded_change')
            diff = '\n'.join(difflib.unified_diff(before, Path(selected['candidate']).read_text().splitlines(),
                                                fromfile='shared-initial-checker', tofile='selected-' + arm, n=3))
            record['arms'][arm] = {'selected': selected, 'relative_to_own_initial': own,
                                   'cell_result_sha256': sha(cell / 'RESULT.json'),
                                   'stop_reason': result['stop_reason'], 'attempts': attempts,
                                   'initial_to_selected_cpp_diff': diff}
        rows.append(record)
    output = {'status': 'recorded_comparative_case_code_ledger', 'main_subjects': 39,
              'comparative_difference_cases': len(rows), 'rows': rows,
              'bindings': {str(ROOT / 'FINAL39_SUMMARY.json'): sha(ROOT / 'FINAL39_SUMMARY.json'),
                           str(ROOT / 'REPEATED_ABLATION_STATUS.json'): sha(ROOT / 'REPEATED_ABLATION_STATUS.json'),
                           str(Path(__file__)): sha(Path(__file__))},
              'boundary': 'All11 non-tied main comparisons, not a selected favorable set. Source differences and recorded adoption/termination only; causal explanations and target correspondence require qualified interpretation. Comparative disadvantage is not necessarily regression from an arm own initial checker. No new model call or target-location scan.'}
    with (ROOT / 'COMPARATIVE_CASE_CODE_LEDGER.json').open('x') as handle:
        json.dump(output, handle, indent=2)
    print(json.dumps({'cases': len(rows), 'benefits': sum(r['comparison_label'] == 'native_comparative_benefit' for r in rows),
                      'disadvantages': sum(r['comparison_label'] == 'native_comparative_disadvantage' for r in rows)}))
