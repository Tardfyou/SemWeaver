"""Relocate verified evidence into the existing consumer's canonical archive."""
import hashlib
import json
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PROJECT = ROOT.parent
OLD = ROOT / 'arm64-evidence'
NEW = PROJECT / 'LLM-Native/artifacts/fse_revision/arm64_corrected_g04_v43'


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def relocate(value):
    if isinstance(value, str):
        return value.replace(str(OLD), str(NEW))
    if isinstance(value, list):
        return [relocate(item) for item in value]
    if isinstance(value, dict):
        return {key: relocate(item) for key, item in value.items()}
    return value


if __name__ == '__main__':
    assert not NEW.exists()
    shutil.copytree(OLD, NEW)
    original_bundle = OLD / 'csa/evidence_bundle.json'
    bundle = NEW / 'csa/evidence_bundle.json'
    payload = relocate(json.loads(original_bundle.read_text()))
    with bundle.open('w') as handle:
        json.dump(payload, handle, indent=2)
        handle.write('\n')
    raw = {}
    for record in payload['records']:
        if record.get('provenance', {}).get('origin') != 'analyzer_internal':
            continue
        fields = record['semantic_payload']
        path = Path(fields['raw_output_path'])
        assert path.is_relative_to(NEW) and '/fse_revision/' in str(path)
        assert sha(path) == fields['raw_output_sha256']
        raw[str(path)] = sha(path)
    assert raw
    manifest = {'original_bundle_sha256': sha(original_bundle), 'relocated_bundle_sha256': sha(bundle),
                'original_collection_manifest_sha256': sha(OLD / 'EVIDENCE_REPLAY_MANIFEST.json'),
                'raw_output_hashes': raw, 'script_sha256': sha(__file__),
                'boundary': 'Only raw-output path prefixes changed to the actual copied archive. '
                            'Native record content and raw output bytes are not fabricated. Original archive retained.'}
    with (NEW / 'RELOCATION_MANIFEST.json').open('x') as handle:
        json.dump(manifest, handle, indent=2)
        handle.write('\n')
    print(json.dumps({'archive': str(NEW), 'verified_native_raw_files': len(raw)}))
