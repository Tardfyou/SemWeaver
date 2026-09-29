"""Monitor the complete cohort without scoring pending or wrong-scope rows."""
import datetime
import json
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PREVIOUS = ROOT.parent / 'native-state-20260928'
sys.path.insert(0, str(PREVIOUS))
import check_all_progress as integrity
integrity.ROOT = ROOT.parent


def snapshot():
    freeze = integrity.read(ROOT / 'MATRIX_FREEZE.json')
    reused = {row['sample_id']: row for row in freeze['reused_pairs']}
    rows = []
    for index, case in enumerate(freeze['subjects']):
        arms = {}
        for arm in ('native', 'no_internal'):
            if case in reused:
                path = PREVIOUS / reused[case][arm]['path']
            else:
                repair = ROOT / 'repairs' / f'{index:02d}-{arm}'
                path = repair if (repair / 'RUN_PLAN.json').exists() else ROOT / 'runs' / f'{index:02d}-{arm}'
            cell = integrity.inspect_cell(path)
            if case.startswith('G04_'):
                corrected = ROOT / 'architecture-corrected' / f'{index:02d}-{arm}'
                witness_repair = ROOT / 'architecture-corrected' / f'{index:02d}-{arm}-witness'
                if (witness_repair / 'RUN_PLAN.json').exists():
                    corrected = witness_repair
                bound_repair = ROOT / 'architecture-corrected' / f'{index:02d}-{arm}-bound'
                if (bound_repair / 'RUN_PLAN.json').exists():
                    corrected = bound_repair
                if (corrected / 'RUN_PLAN.json').exists():
                    path = corrected
                    cell = integrity.inspect_cell(path)
                else:
                    cell.update(verified_complete=False, status='needs_correct_arm64_replay')
            cell['path'] = str(path)
            arms[arm] = cell
        rows.append({'sample_id': case, **arms,
                     'complete_pair': all(cell['verified_complete'] for cell in arms.values())})
    return {'checked_at': datetime.datetime.now(datetime.timezone.utc).isoformat(),
            'planned_samples': 39, 'verified_pairs': sum(row['complete_pair'] for row in rows),
            'rows': rows, 'boundary': 'All cases remain experimental samples. Pending and '
            'wrong-architecture measurements are unscored, never substituted with zero effect.'}


if __name__ == '__main__':
    deadline = integrity.read(ROOT / 'MATRIX_FREEZE.json')['deadline_epoch']
    last = None
    while True:
        state = snapshot()
        temporary = ROOT / 'MONITOR_STATUS.tmp'
        with temporary.open('w') as handle:
            json.dump(state, handle, indent=2)
            handle.write('\n')
        temporary.replace(ROOT / 'MONITOR_STATUS.json')
        compact = [{'sample': row['sample_id'],
                    'native': {k: row['native'][k] for k in ('status', 'model_responses', 'retained_alerts')},
                    'control': {k: row['no_internal'][k] for k in ('status', 'model_responses', 'retained_alerts')}}
                   for row in state['rows'] if row['complete_pair'] or any(
                       row[arm]['status'] != 'queued' for arm in ('native', 'no_internal'))]
        signature = json.dumps(compact, sort_keys=True)
        if signature != last:
            with (ROOT / 'MONITOR_EVENTS.jsonl').open('a') as handle:
                handle.write(json.dumps(state) + '\n')
            print(json.dumps({'checked_at': state['checked_at'], 'verified_pairs': state['verified_pairs'],
                              'planned_samples': 39, 'active_or_closed': compact}), flush=True)
            last = signature
        if state['verified_pairs'] == 39 or time.time() >= deadline:
            break
        time.sleep(min(60, max(1, deadline-time.time())))
