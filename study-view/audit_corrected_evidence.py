"""Recount and validate all39 persisted bundles after the ARM64 correction."""
import collections
import hashlib
import importlib.util
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PROJECT = ROOT.parent
DATA = PROJECT / 'LLM-Native/artifacts/fse_revision'
SOURCE = PROJECT / 'LLM-Native/SemWeaver-v43'


def read(path): return json.loads(path.read_text())
def sha(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()


if __name__ == '__main__':
    spec = importlib.util.spec_from_file_location('original_binding_audit', SOURCE / 'experiments/knighter/experiment/audit_csa_evidence_binding.py')
    audit = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(audit)
    subjects = read(ROOT / 'MATRIX_FREEZE.json')['subjects']
    totals, rows = collections.Counter(), []
    for case in subjects:
        corrected = case.startswith('G04_')
        folder = DATA / 'arm64_corrected_g04_v43' if corrected else DATA / 'generate_only_evidence_all39_v8_r5' / case
        bundle_path = folder / 'csa/evidence_bundle.json'
        manifest_path = folder / ('RELOCATION_MANIFEST.json' if corrected else 'EVIDENCE_REPLAY_MANIFEST.json')
        manifest = read(manifest_path)
        expected = manifest['relocated_bundle_sha256'] if corrected else manifest['evidence_bundle_sha256']
        assert sha(bundle_path) == expected, case
        bundle = read(bundle_path)
        patch = DATA / 'generate_only_materialized_v1/cases' / case / 'patches/commit.patch'
        files, functions = audit.patch_scope(patch)
        counts = collections.Counter()
        internal_bindings = []
        for record in bundle['records']:
            origin = record.get('provenance', {}).get('origin', 'unknown')
            counts[origin] += 1
            if origin != 'analyzer_internal': continue
            payload, scope = record['semantic_payload'], record['scope']
            assert all(payload.get(key) for key in audit.REQUIRED_INTERNAL_FIELDS), case
            assert scope['file'] in files and scope['function'], case
            if functions.get(scope['file']): assert scope['function'] in functions[scope['file']], case
            raw = audit.resolve_recorded_path(payload['raw_output_path'], PROJECT / 'LLM-Native')
            assert raw.is_file() and sha(raw) == payload['raw_output_sha256'], case
            internal_bindings.append({'evidence_id': record['evidence_id'], 'interface': payload['interface'],
                                      'schema': payload['output_schema'], 'raw_sha256': sha(raw),
                                      'file': scope['file'], 'function': scope['function']})
        totals.update(counts)
        rows.append({'case_id': case, 'bundle_sha256': sha(bundle_path),
                     'binding_manifest_sha256': sha(manifest_path), 'ARM64_corrected': corrected,
                     'origin_counts': dict(counts), 'eligible': bool(internal_bindings),
                     'verified_internal_bindings': internal_bindings})
    assert len(rows) == 39
    output = {'status': 'all_persisted_record_bindings_verified', 'samples': 39,
              'eligible_samples': sum(row['eligible'] for row in rows),
              'origin_counts': dict(totals), 'total_records': sum(totals.values()), 'rows': rows,
              'audit_script_sha256': sha(__file__),
              'boundary': 'Persisted initial evidence bundles only. Fresh per-attempt checker execution '
                          'observations, unavailable/cached coverage and observation costs need a separate ledger. '
                          'Coverage is not efficacy or path/ownership proof; G04 replaces the wrong-architecture extraction.'}
    with (ROOT / 'CORRECTED_EVIDENCE_AVAILABILITY.json').open('x') as handle:
        json.dump(output, handle, indent=2)
        handle.write('\n')
    print(json.dumps({key: output[key] for key in ('status','samples','eligible_samples','origin_counts','total_records')}))
