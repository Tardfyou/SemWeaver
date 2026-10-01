"""Offline R0/R1 check of the published repeated-model/source-replay supplement."""
import hashlib
import json
from pathlib import Path
import sys

def require(condition, message):
    if not condition:
        raise ValueError(message)

def main(root):
    root = Path(root).resolve()
    manifest = json.loads((root / 'EXPORT_MANIFEST.json').read_text())
    require(manifest['schema'] == 'semweaver-repeat-source-replay-export.v1', 'Wrong export schema')
    entries = manifest['entries']
    paths = set()
    for row in entries:
        name = row['path']
        require(name not in paths and not Path(name).is_absolute() and '..' not in Path(name).parts,
                'Duplicate or unsafe path')
        paths.add(name)
        path = root / name
        require(path.is_file() and not path.is_symlink(), 'Missing public file: ' + name)
        body = path.read_bytes()
        require(len(body) == row['bytes'] and hashlib.sha256(body).hexdigest() == row['sha256'],
                'Changed public bytes: ' + name)
    summary = json.loads((root / 'evidence/E4_REPEATED_SUMMARY.json').read_text())
    require((summary['total_runs'], summary['new_runs'], summary['reused_first_runs'],
             summary['underlying_subjects']) == (108, 72, 36, 12), 'Wrong E4 scope')
    scored = {}
    for model, group in summary['per_model'].items():
        require(len(group['repeats']) == 3, 'Wrong repeat count: ' + model)
        values = []
        for record in group['repeats']:
            m = record['metrics']
            require(m['cases'] == len(record['rows']) == 12, 'Wrong subject count')
            require((m['TP'] + m['FN'], m['FP'] + m['TN']) == (12, 12), 'Wrong decisions')
            f1 = 2*m['TP']/(2*m['TP']+m['FP']+m['FN'])
            require(abs(f1-m['F1']) < 1e-12, 'F1 mismatch')
            values.append(round(f1, 3))
        scored[model] = values
    require(scored == {'gpt-6-luna': [.560, .615, .500],
                       'glm-5.3': [.583, .615, .667],
                       'glm-5.3-flash': [.692, .560, .667]}, 'Paper/model scores differ')
    roots = list((root / 'evidence/e4-new').glob('*/repeat-*/*/RESULT.json'))
    require(len(roots) == 72, 'Missing new E4 trial result')
    replay = json.loads((root / 'evidence/refined-replay-v2/SOURCE_REPLAY_SUMMARY.json').read_text())
    require(replay['completed_arm_side_cells'] == 72 and replay['unique_scans'] == 64,
            'Replay scan accounting mismatch')
    require(len(replay['rows']) == 36, 'Wrong replay row count')
    native = {(r['case_id'].split('_')[0], r['variant']):
              (r['vulnerable_alerts'], r['fixed_alerts']) for r in replay['rows'] if r['arm'] == 'native'}
    require(native[('G01', 'original')] == (2, 2) and native[('G01', 'guard')] == (0, 0),
            'G01 transition mismatch')
    require(native[('G08', 'original')] == (1, 1) and native[('G08', 'guard')] == (2, 2),
            'G08 transition mismatch')
    require(native[('G09', 'original')] == native[('G09', 'guard')] == (1, 1),
            'G09 transition mismatch')
    print(json.dumps({'status': 'recorded_reanalysis_pass', 'public_files': len(entries),
                      'new_e4_trials': len(roots), 'e4_F1': scored,
                      'replay_arm_side_cells': 72, 'replay_unique_scans': 64}))

if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('usage: verify_recorded.py RESTORED_ROOT')
    main(sys.argv[1])
