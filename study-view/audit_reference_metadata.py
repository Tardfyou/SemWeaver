"""Publisher-deposited DOI metadata audit; no manuscript changes or model calls.

This checks identity/title/year/page metadata only. It neither establishes that
the cited technical claims are supported nor treats network errors as absence.
"""
from concurrent.futures import ThreadPoolExecutor
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import unicodedata
from urllib.parse import quote
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parent
PAPER = ROOT.parent / 'manuscript-original-rebase'


def entries(text):
    """Brace-aware parser for this bibliography's braced fields."""
    found = []
    for match in re.finditer(r'@([A-Za-z]+)\s*\{\s*([^,\s]+)\s*,', text):
        start, depth, i = match.end(), 1, match.end()
        while i < len(text) and depth:
            if text[i] in '{}':
                depth += 1 if text[i] == '{' else -1
            i += 1
        if depth:
            raise ValueError('Unclosed bibliography entry')
        body = text[start:i-1]
        fields = {}
        offset = 0
        while offset < len(body):
            field = re.search(r'\b([A-Za-z_-]+)\s*=\s*\{', body[offset:])
            if not field:
                break
            value_start = offset + field.end()
            end, field_depth = value_start, 1
            while end < len(body) and field_depth:
                if body[end] in '{}':
                    field_depth += 1 if body[end] == '{' else -1
                end += 1
            if field_depth:
                raise ValueError('Unclosed bibliography field')
            fields[field.group(1).lower()] = body[value_start:end-1]
            offset = end
        found.append({'key': match.group(2), 'type': match.group(1), **fields})
    if len(found) != len({r['key'] for r in found}):
        raise ValueError('Duplicate bibliography keys')
    return found


def normalized_title(text):
    text = re.sub(r'<[^>]*>', '', text)
    text = text.replace('\\&', '&')
    text = re.sub(r'\\[\'"`^~=.]', '', text)
    text = re.sub(r'\\(?:texttt|emph|textit|mathrm)', '', text)
    text = unicodedata.normalize('NFKD', text).encode('ascii', 'ignore').decode().lower()
    return re.sub(r'[^a-z0-9]', '', text)


def fetch(entry):
    doi = entry['doi'].strip()
    source = 'https://api.crossref.org/works/' + quote(doi, safe='/')
    request = Request(source, headers={'User-Agent': 'SemWeaver-reference-metadata-audit/1.0'})
    try:
        with urlopen(request, timeout=25) as response:
            raw = response.read()
        message = json.loads(raw)['message']
        titles = message.get('title', [])
        subtitles = message.get('subtitle', [])
        years = sorted({message[name]['date-parts'][0][0] for name in
                        ('issued', 'published', 'published-print', 'published-online')
                        if message.get(name, {}).get('date-parts') and message[name]['date-parts'][0]})
        complete_titles = titles + [title + ': ' + subtitle for title in titles for subtitle in subtitles]
        title_ok = normalized_title(entry.get('title', '')) in {normalized_title(t) for t in complete_titles}
        year_ok = int(entry['year']) in years
        pages = entry.get('pages')
        pages_ok = pages is None or pages.replace('--', '-').replace(' ', '') == message.get('page', '').replace(' ', '')
        return {'key': entry['key'], 'doi': doi, 'source': source,
                'status': 'metadata_matches' if title_ok and year_ok and pages_ok else 'manual_metadata_review_needed',
                'comparisons': {'title': title_ok, 'year': year_ok, 'pages': pages_ok},
                'recorded': {k: entry.get(k) for k in ('title', 'year', 'pages', 'booktitle', 'journal')},
                'publisher_metadata': {k: message.get(k) for k in ('DOI', 'title', 'subtitle', 'container-title', 'author',
                                                                  'type', 'page', 'volume', 'issue', 'URL')},
                'publisher_years': years, 'response_sha256': hashlib.sha256(raw).hexdigest()}
    except Exception as error:
        return {'key': entry['key'], 'doi': doi, 'source': source,
                'status': 'retrieval_failed_not_evidence_of_absence', 'error_type': type(error).__name__}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--recheck', type=Path, help='Recheck only manual-review DOI rows from an immutable earlier checkpoint')
    parser.add_argument('--output', type=Path, default=ROOT / 'REFERENCE_DOI_METADATA_CHECK.json')
    args = parser.parse_args()
    bib = PAPER / 'references.bib'
    raw = bib.read_bytes()
    inventory = entries(raw.decode())
    citations = set()
    for path in PAPER.rglob('*.tex'):
        for group in re.findall(r'\\cite(?:t|p|author|year)?(?:\[[^]]*\])*\{([^}]+)\}', path.read_text()):
            citations.update(k.strip() for k in group.split(','))
    missing = citations - {r['key'] for r in inventory}
    if missing:
        raise ValueError('Missing citation keys')
    used = [r for r in inventory if r['key'] in citations]
    jobs = [r for r in used if r.get('doi')]
    prior = None
    if args.recheck:
        previous = args.recheck.read_bytes()
        prior = json.loads(previous)
        if prior['bibliography_sha256'] != hashlib.sha256(raw).hexdigest():
            raise ValueError('Bibliography changed since checkpoint')
        keys = {r['key'] for r in prior['rows'] if r['status'] == 'manual_metadata_review_needed'}
        jobs = [r for r in jobs if r['key'] in keys]
    with ThreadPoolExecutor(max_workers=4) as executor:
        rows = list(executor.map(fetch, jobs))
    result = {'status': 'doi_metadata_checkpoint_not_claim_verification',
              'checked_at': datetime.now(timezone.utc).isoformat(),
              'bibliography_sha256': hashlib.sha256(raw).hexdigest(), 'entries': len(inventory),
              'used_keys': len(citations), 'doi_entries_checked': len(rows), 'rows': rows,
              'used_non_doi_entries': [{k: r.get(k) for k in ('key', 'title', 'year', 'url', 'howpublished')}
                                       for r in used if not r.get('doi')],
              'boundary': 'Crossref publisher-deposited metadata only. Technical claim support and non-DOI sources need separate primary-source checks. No guessed metadata or bibliography edits.'}
    result['driver_sha256'] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    if prior:
        result['prior_checkpoint_sha256'] = hashlib.sha256(previous).hexdigest()
        result['recheck_reason'] = 'Include separately deposited publisher subtitles; do not shorten valid titles to force a match.'
    output = args.output.resolve()
    with output.open('x') as handle:
        json.dump(result, handle, indent=2)
    print(json.dumps({'used': len(citations), 'doi_checked': len(rows),
                      'matching': sum(r['status'] == 'metadata_matches' for r in rows),
                      'review_needed': [r['key'] for r in rows if r['status'] == 'manual_metadata_review_needed'],
                      'retrieval_failed': [r['key'] for r in rows if r['status'].startswith('retrieval_failed')]}))
