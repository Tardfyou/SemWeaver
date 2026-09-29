"""Bind G24's warning reduction without inventing target-retention evidence."""
import hashlib
import json
import re
from pathlib import Path

ROOT=Path(__file__).resolve().parent
PROJECT=ROOT.parent
CASE='G24_aec8e6bf8391_UAF'
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()

if __name__=='__main__':
    original=PROJECT/'LLM-Native/artifacts/fse_revision/generate_only_materialized_v1/cases'/CASE/'csa/SAGenTestChecker.cpp'
    cell=ROOT/'runs/23-native';result=json.loads((cell/'RESULT.json').read_text())
    selected=result['selected'];candidate=Path(selected['candidate'])
    assert sha(candidate)==selected['candidate_sha256']
    control=json.loads((ROOT/'runs/23-no_internal/RESULT.json').read_text())['selected']
    assert control['candidate_sha256']==sha(original)
    validation=Path(selected['origin']).parent;reports=[]
    for side in ('vulnerable','fixed'):
        for path in (validation/side).rglob('report-*.html'):
            text=path.read_text(errors='replace');fields={}
            for name in ('BUGFILE','BUGLINE','BUGCOLUMN','BUGDESC','BUGTYPE'):
                match=re.search('<!-- '+name+' (.*?) -->',text)
                fields[name]=match.group(1) if match else None
            reports.append({'side':side,'path':str(path),'sha256':sha(path),'fields':fields})
    audit={'case_id':CASE,'candidate_sha256':sha(candidate),'original_sha256':sha(original),
           'selected_validation_sha256':sha(Path(selected['origin'])),
           'retained_native':[selected['vulnerable_alerts'],selected['fixed_alerts']],
           'retained_control':[control['vulnerable_alerts'],control['fixed_alerts']],
           'observed_model_generated_changes':['Check current field SVal for zero before reporting',
                'Use checkBind Loc.getAsRegion() instead of requiring an assignment MemberExpr',
                'Suppress inlined-callee end-function reporting; report at the outer analyzed frame'],
           'reports':reports,'qualification':'version_warning_reduction_target_retention_unconfirmed',
           'boundary':'Actual fixed warning reduction27->2 with vulnerable version still warning-positive. Remaining warnings are outer returns at2340/2366 (fixed shifted+1), not the patch line near1105. Frame-boundary suppression changes diagnostic scope; this is not yet proof that the patched bdev_file misuse remains detected. No report or negative result excluded.'}
    path=ROOT/'QUALIFICATION_G24.json'
    with path.open('x') as handle:json.dump(audit,handle,indent=2)
    print(json.dumps({'case':CASE,'raw_fixed_warning_reduction':25,'target_retention_verified':False}))
