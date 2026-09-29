"""Choose one completed implementation, never best-per-case source mixtures."""
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PREVIOUS = ROOT.parent / 'native-state-20260928'
sys.path.insert(0, str(PREVIOUS))
from check_all_progress import inspect_cell, read, sha


def main():
    paths = [PREVIOUS / 'completed-comparisons' / (name + '.json')
             for name in ('state_key', 'wrapper_matching')]
    matrices = [read(p) for p in paths]
    scores = []
    for matrix in matrices:
        assert matrix['completed_pairs'] == matrix['planned_pairs'] == len(matrix['rows']) == 6
        for row in matrix['rows']:
            for arm in ('native', 'no_internal'):
                current = inspect_cell(PREVIOUS / row[arm]['path'])
                assert current['verified_complete']
                assert current['candidate_sha256'] == row[arm]['candidate_sha256']
                assert current['retained_alerts'] == row[arm]['retained_alerts']
        gains = [r['sample_id'] for r in matrix['rows'] if r['candidate_qualification']
                 and r['candidate_qualification'].get('qualified_semantic_gain')]
        losses = [r['sample_id'] for r in matrix['rows'] if r['raw_comparison'] == 'lost_vulnerable_signal']
        scores.append({'configuration': matrix['configuration'], 'qualified_gain_samples': gains,
                       'lost_signal_samples': losses, 'pairs': 6})
    selected_index = max(range(len(scores)), key=lambda i: (
        -len(scores[i]['lost_signal_samples']), len(scores[i]['qualified_gain_samples'])))
    chosen = matrices[selected_index]
    assert chosen['configuration'] == 'wrapper_matching'
    revisions = {row['native']['source_revision'] for row in chosen['rows']}
    assert revisions == {'16b57931fb105e122897fb1e063e11c366e7137e'}
    decision = {'status': 'whole_implementation_selected', 'configuration': chosen['configuration'],
                'source_revision': revisions.pop(), 'comparison_scores': scores,
                'comparison_hashes': {str(p): sha(p) for p in paths},
                'selection_script_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                'plan_sha256': sha(ROOT / 'PLAN.md'), 'reused_pairs': chosen['rows'],
                'control_reuse_audit_sha256': sha(PREVIOUS / 'CONTROL_REUSE_AUDIT.json'),
                'final39_input_inventory_sha256': sha(PREVIOUS / 'FINAL39_INPUT_INVENTORY.json'),
                'boundary': 'Both configuration tables include all six samples. Choice is by '
                            'qualified measured improvement with no signal losses, not raw scope-contraction '
                            'counts or cross-version pooling. All39 cases remain in the final cohort.'}
    with (ROOT / 'SELECTION.json').open('x') as handle:
        json.dump(decision, handle, indent=2)
        handle.write('\n')
    print(json.dumps({k: decision[k] for k in ('status', 'configuration', 'source_revision', 'comparison_scores')}))


if __name__ == '__main__':
    main()
