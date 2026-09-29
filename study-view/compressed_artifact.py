"""Lossless, content-addressed review distribution; no network or API calls.

Pack only an already exported package. Unpack only into a new directory. Equal
bytes are stored once but each original logical path is restored as a normal
file, so no alias or trial disappears. This format is not a scientific scorer.
"""
from __future__ import annotations

import argparse
from collections import defaultdict
import gzip
import hashlib
import io
import json
from pathlib import Path
import shutil
import tarfile
import tempfile

SCHEMA = 'semweaver.lossless_distribution.v1'
MAX_FILE = 95 * 1024 * 1024
SHARD_PAYLOAD = 64 * 1024 * 1024


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024*1024), b''):
            h.update(chunk)
    return h.hexdigest()


def safe(root, relative):
    p = Path(relative)
    require(not p.is_absolute() and '..' not in p.parts and p.parts and
            p.as_posix() == relative, 'Unsafe distribution path')
    result = root / p
    require(result.resolve().is_relative_to(root.resolve()), 'Distribution path escapes root')
    return result


def public_manifest(root):
    manifest = json.loads((root / 'ARTIFACT_MANIFEST.json').read_text())
    records = list(manifest['files'])
    records.append({'path': 'ARTIFACT_MANIFEST.json', 'size': (root / 'ARTIFACT_MANIFEST.json').stat().st_size,
                    'sha256': sha(root / 'ARTIFACT_MANIFEST.json')})
    require(len(records) == len({r['path'] for r in records}), 'Duplicate artifact path')
    require(manifest['file_count'] + 1 == len(records), 'Incomplete artifact manifest')
    actual = {p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file()}
    require(actual == {r['path'] for r in records}, 'Unlisted artifact file')
    require(not any(p.is_symlink() for p in root.rglob('*')), 'Source package aliases refused')
    for row in records:
        path = safe(root, row['path'])
        require(path.stat().st_size == row['size'] and sha(path) == row['sha256'], 'Source byte mismatch')
        require(row['size'] <= MAX_FILE, 'Source file exceeds distribution bound')
    return records


def browsable(path):
    """Small entry docs, code and canonical summaries remain browsable."""
    p = Path(path)
    if len(p.parts) == 1 or p.parts[0] in {'licenses', 'source-versions'}:
        return True
    if p.suffix == '.py' or p.parts[:3] == ('project', 'LLM-Native', 'SemWeaver-v43'):
        return True
    phase = ('project', 'final-native-20260928')
    if p.parts[:2] == phase:
        return len(p.parts) == 3 or len(p.parts) > 3 and p.parts[2] == 'main39-tables'
    return False


def pack(source, output, shard_payload=SHARD_PAYLOAD):
    source, output = Path(source).resolve(), Path(output).resolve()
    require(not output.exists() and not output.is_relative_to(source), 'Refusing existing/nested distribution')
    require(0 < shard_payload <= SHARD_PAYLOAD, 'Unsafe shard size')
    records = public_manifest(source)
    output.mkdir(parents=True)
    blobs = {}
    files = []
    for record in sorted(records, key=lambda r: r['path']):
        row = dict(record)
        if browsable(row['path']):
            row['storage'] = 'direct'
            target = safe(output, row['path'])
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(safe(source, row['path']), target)
            require(sha(target) == row['sha256'], 'Direct copy changed')
        else:
            row['storage'] = 'object'
            require(row['sha256'] not in blobs or blobs[row['sha256']]['size'] == row['size'], 'Hash/size collision')
            blobs.setdefault(row['sha256'], record)
        files.append(row)
    chunks, current, size = [], [], 0
    for key in sorted(blobs):
        blob = blobs[key]
        if current and size + blob['size'] > shard_payload:
            chunks.append(current)
            current, size = [], 0
        current.append(key)
        size += blob['size']
    if current:
        chunks.append(current)
    descriptors = []
    object_sizes = {key: record['size'] for key, record in blobs.items()}
    for index, keys in enumerate(chunks):
        name = f'evidence-shards/part-{index:04d}.tar.gz'
        target = safe(output, name)
        target.parent.mkdir(exist_ok=True)
        with target.open('xb') as stream:
            with gzip.GzipFile(fileobj=stream, mode='wb', filename='', mtime=0, compresslevel=6) as zipped:
                with tarfile.open(fileobj=zipped, mode='w|', format=tarfile.PAX_FORMAT) as archive:
                    for key in keys:
                        blob = blobs[key]
                        raw = safe(source, blob['path']).read_bytes()
                        require(hashlib.sha256(raw).hexdigest() == key, 'Source changed during compression')
                        item = tarfile.TarInfo('objects/' + key)
                        item.size, item.mode, item.mtime = len(raw), 0o644, 0
                        archive.addfile(item, io.BytesIO(raw))
        require(target.stat().st_size <= MAX_FILE, 'Compressed shard exceeds upload-file bound')
        descriptors.append({'path': name, 'size': target.stat().st_size, 'sha256': sha(target), 'objects': keys})
    helper = Path(__file__).resolve()
    shutil.copyfile(helper, output / 'unpack_artifact.py')
    index = {'schema': SCHEMA, 'files': files, 'objects': object_sizes, 'shards': descriptors,
             'logical_bytes': sum(r['size'] for r in files), 'helper_sha256': sha(output / 'unpack_artifact.py'),
             'boundary': 'Lossless storage transformation only; every logical evidence path and byte is restored.'}
    (output / 'DISTRIBUTION_INDEX.json').write_text(json.dumps(index, indent=2)+'\n')
    (output / 'DISTRIBUTION_README.md').write_text(
        '# Compressed evidence distribution\n\n'
        'Source code, summaries and entry documentation are browsable. Full raw\n'
        'evidence is stored losslessly in hash-addressed compressed shards. Equal\n'
        'bytes are stored once; all original paths and trials are restored.\n\n'
        'Unpack into a new directory, then verify all recorded study results:\n\n'
        '```sh\npython3 unpack_artifact.py unpack . --output ../semweaver-expanded\n'
        'python3 ../semweaver-expanded/project/final-native-20260928/verify_final_artifact.py ../semweaver-expanded\n```\n\n'
        f'Unpacking requires approximately {index["logical_bytes"]/1024**3:.2f} GiB of disk space '
        'plus temporary object space. It uses no network, keys or model calls.\n'
        'Integrity verification does not certify scientific claims or anonymity.\n')
    distribution = []
    for path in sorted(output.rglob('*')):
        if path.is_file():
            distribution.append({'path': path.relative_to(output).as_posix(),
                                 'size': path.stat().st_size, 'sha256': sha(path)})
    (output / 'DISTRIBUTION_MANIFEST.json').write_text(json.dumps({'schema': SCHEMA,
                'files': distribution}, indent=2)+'\n')
    return index


def verify_distribution(root):
    manifest = json.loads((root / 'DISTRIBUTION_MANIFEST.json').read_text())
    require(manifest['schema'] == SCHEMA, 'Unknown distribution schema')
    expected = set()
    for row in manifest['files']:
        require(row['path'] not in expected, 'Duplicate distribution file')
        expected.add(row['path'])
        path = safe(root, row['path'])
        require(path.is_file() and not path.is_symlink() and path.stat().st_size == row['size']
                and sha(path) == row['sha256'], 'Distribution byte mismatch')
    require(not any(p.is_symlink() for p in root.rglob('*')), 'Distribution aliases refused')
    actual = {p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file()}
    require(actual == expected | {'DISTRIBUTION_MANIFEST.json'}, 'Unlisted distribution files')
    index = json.loads((root / 'DISTRIBUTION_INDEX.json').read_text())
    require(index['schema'] == SCHEMA, 'Unknown evidence index schema')
    require(sha(root / 'unpack_artifact.py') == index['helper_sha256'], 'Unpack helper binding mismatch')
    return index


def unpack(source, output):
    source, output = Path(source).resolve(), Path(output).resolve()
    require(not output.exists() and not output.is_relative_to(source), 'Refusing existing/nested unpack directory')
    index = verify_distribution(source)
    paths = set()
    expected_objects = {}
    by_object = defaultdict(list)
    for row in index['files']:
        safe(output, row['path'])
        require(row['path'] not in paths and type(row['size']) is int and 0 <= row['size'] <= MAX_FILE,
                'Invalid logical file entry')
        paths.add(row['path'])
        if row['storage'] == 'object':
            key = row['sha256']
            require(len(key) == 64 and all(c in '0123456789abcdef' for c in key), 'Invalid object hash')
            require(key not in expected_objects or expected_objects[key] == row['size'], 'Conflicting object size')
            expected_objects[key] = row['size']
            by_object[key].append(row)
        else:
            require(row['storage'] == 'direct', 'Unknown storage class')
    require(expected_objects == index['objects'], 'Missing/unused evidence objects')
    require(sum(r['size'] for r in index['files']) == index['logical_bytes'], 'Wrong unpack size')
    output.mkdir(parents=True)
    for row in index['files']:
        if row['storage'] == 'direct':
            original, target = safe(source, row['path']), safe(output, row['path'])
            require(original.stat().st_size == row['size'] and sha(original) == row['sha256'], 'Wrong direct bytes')
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(original, target)
    seen = set()
    with tempfile.TemporaryDirectory(prefix='.unpack-objects-', dir=output) as temporary:
        cache = Path(temporary)
        for shard in index['shards']:
            path = safe(source, shard['path'])
            require(path.stat().st_size == shard['size'] and sha(path) == shard['sha256'], 'Shard byte mismatch')
            keys = set(shard['objects'])
            require(len(keys) == len(shard['objects']) and keys.isdisjoint(seen) and
                    keys.issubset(expected_objects), 'Duplicate/unknown shard objects')
            got = set()
            with tarfile.open(path, mode='r|gz') as archive:
                for item in archive:
                    require(item.isfile() and item.name.startswith('objects/'), 'Archive link or invalid member')
                    key = item.name[len('objects/'):]
                    require(key in keys and key not in got and item.size == expected_objects[key],
                            'Unexpected/duplicate archive object')
                    require(item.size <= MAX_FILE, 'Oversized archive object')
                    member = archive.extractfile(item)
                    require(member is not None, 'Unreadable archive member')
                    blob = cache / key
                    with blob.open('xb') as handle:
                        shutil.copyfileobj(member, handle, 1024*1024)
                    require(blob.stat().st_size == item.size and sha(blob) == key, 'Decompressed object mismatch')
                    for row in by_object[key]:
                        target = safe(output, row['path'])
                        target.parent.mkdir(parents=True, exist_ok=True)
                        shutil.copyfile(blob, target)
                    blob.unlink()  # Only this newly generated temporary object.
                    got.add(key)
            require(got == keys, 'Missing shard object')
            seen.update(got)
    require(seen == set(expected_objects), 'Incomplete evidence reconstruction')
    public_manifest(output)
    return {'status': 'lossless_reconstruction_verified', 'files': len(paths), 'bytes': index['logical_bytes']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation', choices=('pack', 'unpack', 'verify'))
    parser.add_argument('source', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.operation == 'verify':
        index = verify_distribution(args.source.resolve())
        print(json.dumps({'status': 'distribution_bytes_verified', 'logical_files': len(index['files'])}))
    else:
        require(args.output is not None, 'Operation requires --output')
        result = pack(args.source, args.output) if args.operation == 'pack' else unpack(args.source, args.output)
        if args.operation == 'pack':
            result = {'status': 'lossless_distribution_written', 'files': len(result['files']),
                      'objects': len(result['objects']), 'shards': len(result['shards'])}
        print(json.dumps(result))


if __name__ == '__main__':
    main()
