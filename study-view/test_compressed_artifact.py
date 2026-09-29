"""Synthetic lossless round-trip and fail-closed distribution tests."""
import hashlib
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest

import compressed_artifact as c


class DistributionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.base = Path(self.temp.name)
        self.source = self.base / 'source'
        self.source.mkdir()
        self.distribution = self.base / 'distribution'
        self.expanded = self.base / 'expanded'
        payloads = {'README.md': b'# Synthetic fixture\n',
                    'project/old-trials/positive/raw.jsonl': b'{"synthetic":true}\n'*100,
                    'project/old-trials/negative/raw.jsonl': b'{"synthetic":true}\n'*100,
                    'project/old-trials/other/raw.jsonl': b'{"other":true}\n'*100}
        records = []
        for name, raw in payloads.items():
            path = self.source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(raw)
            records.append({'path': name, 'size': len(raw), 'sha256': hashlib.sha256(raw).hexdigest()})
        (self.source / 'ARTIFACT_MANIFEST.json').write_text(json.dumps({'files': records, 'file_count': len(records)}))

    def tearDown(self):
        self.temp.cleanup()

    def pack(self):
        return c.pack(self.source, self.distribution, shard_payload=2000)

    def rebind(self):
        rows = []
        for path in sorted(self.distribution.rglob('*')):
            if path.is_file() and path.name != 'DISTRIBUTION_MANIFEST.json':
                rows.append({'path': path.relative_to(self.distribution).as_posix(),
                             'size': path.stat().st_size, 'sha256': c.sha(path)})
        (self.distribution / 'DISTRIBUTION_MANIFEST.json').write_text(json.dumps({'schema': c.SCHEMA, 'files': rows}))

    def mutate_index(self, update):
        path = self.distribution / 'DISTRIBUTION_INDEX.json'
        index = json.loads(path.read_text())
        update(index)
        path.write_text(json.dumps(index))
        self.rebind()

    def test_round_trip_all_bytes_and_paths(self):
        index = self.pack()
        result = c.unpack(self.distribution, self.expanded)
        self.assertEqual(result['files'], 5)
        self.assertEqual(len(index['objects']), 2)
        for path in self.source.rglob('*'):
            if path.is_file():
                restored = self.expanded / path.relative_to(self.source)
                self.assertEqual(path.read_bytes(), restored.read_bytes())
                self.assertFalse(restored.is_symlink())

    def test_same_shard_bytes_on_second_pack(self):
        first = self.pack()
        second_root = self.base / 'second'
        second = c.pack(self.source, second_root, shard_payload=2000)
        self.assertEqual([r['sha256'] for r in first['shards']], [r['sha256'] for r in second['shards']])

    def test_source_tamper_rejected_before_destination_creation(self):
        (self.source / 'README.md').write_bytes(b'changed')
        with self.assertRaises(ValueError):
            self.pack()
        self.assertFalse(self.distribution.exists())

    def test_existing_destination_preserved(self):
        self.pack()
        self.expanded.mkdir()
        marker = self.expanded / 'user-file'
        marker.write_text('preserve')
        with self.assertRaises(ValueError):
            c.unpack(self.distribution, self.expanded)
        self.assertEqual(marker.read_text(), 'preserve')

    def test_nested_destination_rejected(self):
        self.pack()
        with self.assertRaises(ValueError):
            c.unpack(self.distribution, self.distribution / 'expanded')

    def test_shard_tamper_rejected(self):
        index = self.pack()
        (self.distribution / index['shards'][0]['path']).write_bytes(b'changed')
        with self.assertRaises(ValueError):
            c.unpack(self.distribution, self.expanded)

    def test_missing_shard_rejected(self):
        index = self.pack()
        (self.distribution / index['shards'][0]['path']).unlink()
        with self.assertRaises(ValueError):
            c.unpack(self.distribution, self.expanded)

    def test_unsafe_logical_path_rejected(self):
        self.pack()
        self.mutate_index(lambda d: d['files'][0].update(path='../outside'))
        with self.assertRaises(ValueError):
            c.unpack(self.distribution, self.expanded)
        self.assertFalse((self.base / 'outside').exists())

    def test_duplicate_logical_path_rejected(self):
        self.pack()
        self.mutate_index(lambda d: d['files'].append(dict(d['files'][0])))
        with self.assertRaises(ValueError):
            c.unpack(self.distribution, self.expanded)

    def test_unknown_object_rejected(self):
        self.pack()
        self.mutate_index(lambda d: d['objects'].update({'f'*64: 10}))
        with self.assertRaises(ValueError):
            c.unpack(self.distribution, self.expanded)

    def test_missing_descriptor_objects_rejected(self):
        self.pack()
        self.mutate_index(lambda d: d.update(shards=[]))
        with self.assertRaises(ValueError):
            c.unpack(self.distribution, self.expanded)

    def test_tar_symlink_rejected_even_with_rebound_checksums(self):
        index = self.pack()
        path = self.distribution / index['shards'][0]['path']
        with tarfile.open(path, 'w:gz') as archive:
            item = tarfile.TarInfo('objects/' + index['shards'][0]['objects'][0])
            item.type, item.linkname = tarfile.SYMTYPE, '/tmp/outside'
            archive.addfile(item)
        def update(d):
            d['shards'][0].update(size=path.stat().st_size, sha256=c.sha(path))
        self.mutate_index(update)
        with self.assertRaises(ValueError):
            c.unpack(self.distribution, self.expanded)

    def test_tar_unexpected_path_rejected(self):
        index = self.pack()
        path = self.distribution / index['shards'][0]['path']
        with tarfile.open(path, 'w:gz') as archive:
            item = tarfile.TarInfo('../../outside')
            item.size = 3
            archive.addfile(item, io.BytesIO(b'bad'))
        self.mutate_index(lambda d: d['shards'][0].update(size=path.stat().st_size, sha256=c.sha(path)))
        with self.assertRaises(ValueError):
            c.unpack(self.distribution, self.expanded)
        self.assertFalse((self.base / 'outside').exists())


if __name__ == '__main__':
    unittest.main()
