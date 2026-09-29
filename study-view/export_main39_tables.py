"""English, lossless CSV views of the verified full39 aggregation only."""
import csv
import hashlib
import io
import json
from pathlib import Path

ROOT=Path(__file__).resolve().parent


def table(fields,rows):
    stream=io.StringIO(newline='');writer=csv.DictWriter(stream,fieldnames=fields)
    writer.writeheader();writer.writerows(rows);return stream.getvalue()


def make_tables(data):
    assert data['status']=='completed_main_baseline39'
    methods={'unmodified':data['starting'],**data['portfolios']}
    subjects={r['case_id'] for r in data['starting']['rows']};assert len(subjects)==39
    summaries=[];cases=[];subset=[]
    for method,block in methods.items():
        assert len(block['rows'])==39 and {r['case_id'] for r in block['rows']}==subjects
        summaries.append({'method':method,**block['metrics'],
                          'model_responses':block.get('model_responses',0)})
        for row in block['rows']:
            cases.append({'method':method,**{k:row[k] for k in (
                'case_id','vulnerable_alerts','fixed_alerts','model_responses','candidate_sha256')},
                'validation_sha256':row['result_sha256']})
    for method,block in data['matched15'].items():
        assert len(block['rows'])==15
        subset.append({'method':method,**block['metrics']})
    metrics=list(data['starting']['metrics'])
    return {'SUMMARY.csv':table(['method',*metrics,'model_responses'],summaries),
            'CASE_TABLE.csv':table(['method','case_id','vulnerable_alerts','fixed_alerts',
                                   'model_responses','candidate_sha256','validation_sha256'],cases),
            'MATCHED15.csv':table(['method',*metrics],subset)}


if __name__=='__main__':
    source=ROOT/'FINAL39_SUMMARY.json';raw=source.read_bytes()
    tables=make_tables(json.loads(raw));output=ROOT/'main39-tables';output.mkdir(exist_ok=True)
    hashes={}
    for name,content in tables.items():
        data=content.encode();temporary=output/(name+'.tmp');temporary.write_bytes(data);temporary.replace(output/name)
        hashes[name]=hashlib.sha256(data).hexdigest()
    receipt={'status':'verified_main_tables_only','source_summary_sha256':hashlib.sha256(raw).hexdigest(),
             'exporter_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'files':hashes,
             'boundary':'Lossless CSV views, all39 subjects and preexisting15 stratum. Patch-version warning metrics, not target recall or full-study publication readiness. Auxiliary closure is separate.'}
    (output/'TABLE_MANIFEST.json').write_text(json.dumps(receipt,indent=2))
    print(json.dumps({'tables':list(tables),'case_method_rows':156,'publication_ready':False}))
