"""All original12, three paired decodes; partial observations never imputed."""
from collections import Counter
import hashlib
import json
from pathlib import Path
import watch_final

ROOT=Path(__file__).resolve().parent


def direction(native,control):
    if native is None or control is None:return None
    nv,nf=native;cv,cf=control
    if nv>0 and cv==0:return 'positive_warning_recovery'
    if nv>0 and cv>0 and nf<cf:return 'positive_fixed_warning_reduction'
    if cv>0 and nv==0:return 'negative_lost_version_hit'
    if nv>0 and cv>0 and nf>cf:return 'negative_more_fixed_warnings'
    return 'tie_or_other'


def summarize(subjects,main,extra):
    assert len(subjects)==12 and len(set(subjects))==12
    first={p['sample_id']:p for p in main['rows']}
    subsequent={}
    for row in extra['e3']['rows']:
        parts=Path(row['parent']).relative_to(ROOT/'e3-selected-repeats').parts
        repeat=int(parts[0].removeprefix('repeat-'))
        key=(row['case_id'],repeat,row['arm'])
        assert key not in subsequent and repeat in (2,3)
        subsequent[key]=row
    assert len(subsequent)==48
    rows=[];totals=Counter()
    for case in subjects:
        repeats=[]
        for repeat in (1,2,3):
            arms={}
            for arm in ('native','no_internal'):
                cell=first[case][arm] if repeat==1 else subsequent[(case,repeat,arm)]
                complete=cell['verified_complete'] and not cell['integrity_errors']
                arms[arm]={'complete':complete,'alerts':cell['retained_alerts'] if complete else None,
                           'cell':cell.get('path',cell.get('selected_cell')),
                           'model_responses':cell['model_responses'],'integrity_errors':cell['integrity_errors']}
            outcome=direction(arms['native']['alerts'],arms['no_internal']['alerts'])
            if outcome is not None:totals[outcome]+=1
            repeats.append({'repeat':repeat,'paired_complete':outcome is not None,
                            'outcome':outcome,**arms})
        finished=all(r['paired_complete'] for r in repeats)
        signs=[r['outcome'].split('_')[0] for r in repeats if r['paired_complete']]
        rows.append({'case_id':case,'repeats':repeats,'complete_three_pairs':finished,
                     'direction_consistency':('all_positive' if signs==['positive']*3 else
                       'all_negative' if signs==['negative']*3 else
                       'all_tie_or_other' if signs==['tie']*3 else 'mixed') if finished else None,
                     'completed_positive_decodes':signs.count('positive'),
                     'completed_negative_decodes':signs.count('negative')})
    finished=[r for r in rows if r['complete_three_pairs']]
    return {'status':'complete' if len(finished)==12 else 'partial',
            'planned_subjects':12,'planned_paired_decodes':36,'planned_condition_cells':72,
            'complete_paired_decodes':sum(totals.values()),'complete_three_decode_subjects':len(finished),
            'decode_outcome_counts':dict(totals),
            'three_decode_consistency_counts':dict(Counter(r['direction_consistency'] for r in finished)),
            'rows':rows,'boundary':'Same12 experimental subjects, three decodes each, first reused from main. Do not treat36 paired decodes as36 independent bugs. Outcomes are version-warning changes including noisy recoveries, not target recall, significant causal effects, or independent generalization. All negative/tied/incomplete rows retained; incomplete outcomes and consistency are null.'}


if __name__=='__main__':
    subjects=json.loads((ROOT/'E3_E4_PREPARATION.json').read_text())['repeated_samples']
    raw=(ROOT/'REMAINING_MATRIX_STATUS.json').read_bytes()
    result=summarize(subjects,watch_final.snapshot(),json.loads(raw))
    result['auxiliary_snapshot_sha256']=hashlib.sha256(raw).hexdigest()
    result['aggregator_sha256']=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    temporary=ROOT/'REPEATED_ABLATION_STATUS.tmp'
    temporary.write_text(json.dumps(result,indent=2));temporary.replace(ROOT/'REPEATED_ABLATION_STATUS.json')
    print(json.dumps({k:v for k,v in result.items() if k not in ('rows','boundary')}))
