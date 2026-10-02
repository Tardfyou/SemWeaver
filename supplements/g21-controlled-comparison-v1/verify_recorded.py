"""Offline R0/R1 check for the G21 controlled comparison."""
import csv
import hashlib
import json
from pathlib import Path
import sys

SUITES = ('core', 'boundary', 'confirmation', 'transfer_confirmation')
EXPECTED = {
    'knighter-loop': (31, 25),
    'main-one-response': (31, 43),
    'illustrative-14-response': (58, 56),
}

def require(value, message):
    if not value:
        raise ValueError(message)

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main(root):
    root = Path(root).resolve()
    manifest = json.loads((root / 'EXPORT_MANIFEST.json').read_text())
    require(manifest['schema'] == 'semweaver-g21-comparison-export.v1', 'Wrong export schema')
    seen = set()
    for item in manifest['entries']:
        name = item['path']
        require(name not in seen and not Path(name).is_absolute() and '..' not in Path(name).parts,
                'Unsafe or repeated path')
        seen.add(name)
        file = root / name
        require(file.is_file() and not file.is_symlink() and file.stat().st_size == item['bytes']
                and sha(file) == item['sha256'], 'Changed public bytes: ' + name)
    provenance = root / 'BASELINE_LOOP_PROVENANCE.json'
    require(sha(provenance) == manifest['baseline_provenance_sha256'], 'Baseline provenance changed')
    baseline = json.loads(provenance.read_text())
    require(baseline['knighter_original_paired_alerts'] == [1, 1]
            and baseline['original_checker_sha256'] == baseline['knighter_retained_checker_sha256']
            and 'No-FP' in baseline['knighter_stop'], 'Wrong baseline stop')
    scored = {}
    for arm, expected in EXPECTED.items():
        arm_dir = root / 'evidence' / arm
        outcome = json.loads((arm_dir / 'RESULT.json').read_text())
        require(outcome['status'] == 'completed_fixture_replay'
                and outcome['checker_sha256'] == sha(arm_dir / 'SAGenTestChecker.cpp'),
                'Wrong checker/result identity')
        run = json.loads((arm_dir / 'RUN_MANIFEST.json').read_text())
        require(run['status'] == 'completed' and run['inputs_unchanged']
                and run['additional_model_calls'] == 0, 'Incomplete or changed replay')
        positive = negative = total = 0
        for suite in SUITES:
            source = root / 'inputs' / suite
            rows = list(csv.DictReader((source / 'manifest.csv').open()))
            result = json.loads((arm_dir / suite / 'RESULT.json').read_text())
            require(result['checker_sha256'] == outcome['checker_sha256'], 'Checker drift')
            recorded = result['results']
            require(len(rows) == len(recorded), 'Missing fixture result')
            for spec, record in zip(rows, recorded):
                require(spec['variant_id'] == record['variant_id']
                        and spec['expected_alert'] == record['expected_alert']
                        and record['return_code'] == 0, 'Fixture identity or execution failure')
                fixture = source / spec['source_path']
                require(sha(fixture) == record['source_sha256'], 'Fixture changed')
                raw_log = arm_dir / suite / 'raw_logs' / (spec['variant_id'] + '.log')
                require(raw_log.is_file() and
                        raw_log.read_text().count('[custom.SAGenTestChecker]') == record['alert_count'],
                        'Raw checker diagnostics disagree with the result row')
                passed = int(record['observed_alert'] == bool(int(spec['expected_alert'])))
                require(passed == int(record['passed']), 'Recorded verdict mismatch')
                if spec['variant_kind'] == 'positive':
                    positive += passed
                else:
                    require(spec['variant_kind'] == 'negative', 'Unknown fixture class')
                    negative += passed
                total += 1
        require(total == 114 and (positive, negative) == expected,
                'Aggregate comparison mismatch')
        scored[arm] = {'unsafe_warned': positive, 'safe_silent': negative,
                       'total_correct': positive + negative}
    print(json.dumps({'status': 'recorded_reanalysis_pass', 'public_files': len(seen),
                      'controlled_executions': 114, 'scores': scored}))

if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('usage: verify_recorded.py EXPORTED_ROOT')
    main(sys.argv[1])
