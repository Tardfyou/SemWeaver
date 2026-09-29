"""Canonical code ledger with immutable repeated-input bytes.

The earlier ledger bound a changing monitor view. Keep it as history, but never
use that mutable binding to certify the final package. Outcomes are unchanged.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parent


if __name__ == '__main__':
    import json
    snapshot = ROOT / 'COMPARATIVE_CASE_REPEAT_INPUT.json'
    raw = (snapshot if snapshot.exists() else ROOT / 'REPEATED_ABLATION_STATUS.json').read_bytes()
    data = json.loads(raw)
    assert data['status'] == 'complete' and data['complete_paired_decodes'] == 36
    if not snapshot.exists():
        with snapshot.open('xb') as handle:
            handle.write(raw)
    original = ROOT / 'collect_case_mechanisms.py'
    source = original.read_text()
    changes = {
        "ROOT / 'REPEATED_ABLATION_STATUS.json'": "ROOT / 'COMPARATIVE_CASE_REPEAT_INPUT.json'",
        "ROOT / 'COMPARATIVE_CASE_CODE_LEDGER.json'": "ROOT / 'COMPARATIVE_CASE_CODE_LEDGER_V2.json'",
        "str(Path(__file__)): sha(Path(__file__))": "str(original): sha(original), str(Path(__file__)): sha(Path(__file__))",
        "'status': 'recorded_comparative_case_code_ledger'": "'status': 'frozen_recorded_comparative_case_code_ledger'",
    }
    for old, new in changes.items():
        assert source.count(old) == (3 if 'REPEATED_ABLATION_STATUS' in old else 1), 'Case-ledger generator drift'
        source = source.replace(old, new)
    exec(compile(source, str(original) + '[immutable-repeat-input]', 'exec'),
         {'__name__': '__main__', '__file__': str(Path(__file__).resolve()), 'original': original})
