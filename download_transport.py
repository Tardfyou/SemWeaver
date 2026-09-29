"""Download a public review transport through its documented file API.

No authentication or model access. Bytes are checked before restoration.
"""
import argparse
import hashlib
import json
from pathlib import Path
import time
import urllib.error
import urllib.parse
import urllib.request

LIMIT = 7 * 1024 * 1024


def fetch(base, name):
    url = base.rstrip('/')+'/'+urllib.parse.quote(name, safe='/')
    for attempt in range(6):
        try:
            with urllib.request.urlopen(url, timeout=120) as response:
                data = response.read(LIMIT+1)
            if len(data)>LIMIT:
                raise ValueError('Download exceeds file-size bound')
            return data
        except urllib.error.HTTPError as error:
            if error.code not in (429,500,502,503,504) or attempt==5:
                raise
        except (urllib.error.URLError,TimeoutError):
            if attempt==5:
                raise
        time.sleep(min(2**attempt,30))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--api-base', required=True,
                        help='Public URL ending in /api/repo/REVIEW_ID/file')
    parser.add_argument('--output', type=Path, required=True)
    args=parser.parse_args()
    parsed=urllib.parse.urlsplit(args.api_base)
    if (parsed.scheme!='https' or parsed.username or parsed.password or
            parsed.query or parsed.fragment or not parsed.path.rstrip('/').endswith('/file')):
        raise ValueError('HTTPS public file API URL without credentials required')
    if args.output.exists():
        raise ValueError('New output directory required')
    args.output.mkdir(parents=True)
    raw=fetch(args.api_base,'TRANSPORT_INDEX.json')
    index=json.loads(raw)
    if index.get('schema')!='review-byte-transport.v1':
        raise ValueError('Unexpected public index')
    (args.output/'TRANSPORT_INDEX.json').write_bytes(raw)
    helper=fetch(args.api_base,'restore_review.py')
    if hashlib.sha256(helper).hexdigest()!=index['helper_sha256']:
        raise ValueError('Restore helper hash mismatch; service may have modified text')
    (args.output/'restore_review.py').write_bytes(helper)
    for i,part in enumerate(index['parts'],1):
        name=part['path']
        if ('..' in Path(name).parts or Path(name).is_absolute() or
                not name.startswith('payload/part-') or not name.endswith('.bin')):
            raise ValueError('Unsafe part name')
        data=fetch(args.api_base,name)
        if len(data)!=part['size'] or hashlib.sha256(data).hexdigest()!=part['sha256']:
            raise ValueError('Part hash/size mismatch')
        target=args.output/name
        target.parent.mkdir(exist_ok=True)
        target.write_bytes(data)
        print(f'Verified part{i}/{len(index["parts"])}',flush=True)
    print('Public transport downloaded and verified; run restore_review.py next.')
