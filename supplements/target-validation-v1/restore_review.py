"""Lossless small-file review transport. No model, analyzer or network calls."""
import argparse
import gzip
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import shutil
import tarfile

SCHEMA = 'review-byte-transport.v1'
PART_BYTES = 7 * 1024 * 1024
MAX_LOGICAL = 95 * 1024 * 1024
HEADER = 'TRANSFER_FILES.json'


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def safe_name(name):
    p = PurePosixPath(name)
    require(not p.is_absolute() and p.parts and '..' not in p.parts and
            p.as_posix() == name, 'Unsafe/noncanonical archive path')
    return p


class PartWriter:
    def __init__(self, root, limit):
        self.root, self.limit, self.parts = root, limit, []
        self.stream = None
        self.count = 0

    def write(self, data):
        original = len(data)
        view = memoryview(data)
        while view:
            if self.stream is None:
                self.path = self.root / f'payload/part-{len(self.parts):04d}.bin'
                self.path.parent.mkdir(exist_ok=True)
                self.stream = self.path.open('xb')
                self.count = 0
            n = min(len(view), self.limit-self.count)
            self.stream.write(view[:n])
            self.count += n
            view = view[n:]
            if self.count == self.limit:
                self.close_part()
        return original

    def close_part(self):
        if self.stream is not None:
            self.stream.close()
            self.parts.append(dict(path=self.path.relative_to(self.root).as_posix(),
                                   size=self.count, sha256=sha(self.path)))
            self.stream = None

    def flush(self):
        if self.stream is not None:
            self.stream.flush()


class JoinedReader(io.RawIOBase):
    def __init__(self, root, parts):
        self.root, self.parts, self.position, self.stream = root, parts, 0, None

    def readable(self):
        return True

    def readinto(self, buffer):
        while True:
            if self.stream is None:
                if self.position == len(self.parts):
                    return 0
                self.stream = (self.root / self.parts[self.position]['path']).open('rb')
                self.position += 1
            n = self.stream.readinto(buffer)
            if n:
                return n
            self.stream.close()
            self.stream = None

    def close(self):
        if self.stream is not None:
            self.stream.close()
        super().close()


def pack(source, output, limit=PART_BYTES):
    source, output = Path(source).resolve(), Path(output).resolve()
    require(not output.exists() and not output.is_relative_to(source), 'New separate destination required')
    require(0 < limit <= PART_BYTES, 'Unsafe part bound')
    rows = []
    for p in sorted(source.rglob('*')):
        require(not p.is_symlink(), 'Source symlink')
        if p.is_file():
            name = p.relative_to(source).as_posix()
            safe_name(name)
            require(name != HEADER and p.stat().st_size <= MAX_LOGICAL, 'Invalid logical file')
            rows.append(dict(path=name, size=p.stat().st_size, sha256=sha(p)))
    header = (json.dumps({'files': rows}, sort_keys=True)+'\n').encode()
    require(len(header) <= MAX_LOGICAL, 'Oversized transfer map')
    output.mkdir()
    writer = PartWriter(output, limit)
    with gzip.GzipFile(fileobj=writer, mode='wb', filename='', mtime=0, compresslevel=6) as zipped:
        with tarfile.open(fileobj=zipped, mode='w|', format=tarfile.PAX_FORMAT) as archive:
            info = tarfile.TarInfo(HEADER)
            info.size, info.mode, info.mtime = len(header), 0o644, 0
            archive.addfile(info, io.BytesIO(header))
            for row in rows:
                p = source / row['path']
                require(sha(p) == row['sha256'], 'Source changed before transport')
                info = tarfile.TarInfo(row['path'])
                info.size, info.mode, info.mtime = row['size'], 0o644, 0
                with p.open('rb') as stream:
                    archive.addfile(info, stream)
                require(sha(p) == row['sha256'], 'Source changed during transport')
    writer.close_part()
    shutil.copyfile(Path(__file__), output / 'restore_review.py')
    index = dict(schema=SCHEMA, parts=writer.parts, file_map_sha256=hashlib.sha256(header).hexdigest(),
                 file_count=len(rows), logical_bytes=sum(r['size'] for r in rows),
                 helper_sha256=sha(output / 'restore_review.py'), max_part_bytes=limit)
    (output / 'TRANSPORT_INDEX.json').write_text(json.dumps(index, indent=2)+'\n')
    verify(output)
    return index


def verify(source):
    source = Path(source).resolve()
    index = json.loads((source / 'TRANSPORT_INDEX.json').read_text())
    require(index['schema'] == SCHEMA and 0 < index['max_part_bytes'] <= PART_BYTES, 'Wrong transport schema/bound')
    require(sha(source / 'restore_review.py') == index['helper_sha256'], 'Changed restore helper')
    paths = set()
    for part in index['parts']:
        name = safe_name(part['path']).as_posix()
        require(name not in paths and name.startswith('payload/part-') and name.endswith('.bin'), 'Wrong/duplicate part')
        paths.add(name)
        p = source / name
        require(p.is_file() and not p.is_symlink() and 0 < p.stat().st_size == part['size'] <= PART_BYTES and
                sha(p) == part['sha256'], 'Missing/tampered/oversized transport part')
    require({p.relative_to(source).as_posix() for p in (source/'payload').rglob('*') if p.is_file()} == paths,
            'Unlisted transport part')
    return index


def restore(source, output):
    source, output = Path(source).resolve(), Path(output).resolve()
    require(not output.exists() and not output.is_relative_to(source), 'New separate restore destination required')
    index = verify(source)
    output.mkdir()
    seen = set()
    with JoinedReader(source, index['parts']) as joined, io.BufferedReader(joined) as buffered:
        with gzip.GzipFile(fileobj=buffered, mode='rb') as zipped:
            with tarfile.open(fileobj=zipped, mode='r|') as archive:
                first = archive.next()
                require(first is not None and first.isfile() and first.name == HEADER and first.size <= MAX_LOGICAL,
                        'Missing/invalid protected transfer map')
                raw = archive.extractfile(first).read()
                require(hashlib.sha256(raw).hexdigest() == index['file_map_sha256'], 'Transfer map changed')
                rows = json.loads(raw)['files']
                expected = {r['path']: r for r in rows}
                require(len(rows) == len(expected) == index['file_count'] and
                        sum(r['size'] for r in rows) == index['logical_bytes'], 'Wrong transfer map totals')
                for item in archive:
                    if item is first:
                        continue
                    name = safe_name(item.name).as_posix()
                    require(item.isfile() and name in expected and name not in seen and
                            0 <= item.size == expected[name]['size'] <= MAX_LOGICAL, 'Unexpected/unsafe archive member')
                    target = output / name
                    target.parent.mkdir(parents=True, exist_ok=True)
                    stream = archive.extractfile(item)
                    require(stream is not None, 'Unreadable member')
                    with target.open('xb') as destination:
                        shutil.copyfileobj(stream, destination, 1024 * 1024)
                    require(sha(target) == expected[name]['sha256'], 'Restored byte mismatch')
                    seen.add(name)
    require(seen == set(expected), 'Incomplete restored tree')
    return dict(status='exact_transport_restore_verified', files=len(seen), bytes=index['logical_bytes'])


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('operation', choices=('pack', 'restore', 'verify'))
    p.add_argument('source', type=Path)
    p.add_argument('--output', type=Path)
    args = p.parse_args()
    if args.operation == 'verify':
        d = verify(args.source)
        print(json.dumps(dict(status='transport_bytes_verified', parts=len(d['parts']), files=d['file_count'])))
    else:
        require(args.output is not None, 'Output required')
        d = pack(args.source, args.output) if args.operation == 'pack' else restore(args.source, args.output)
        print(json.dumps({k:v for k,v in d.items() if k != 'parts'}))
