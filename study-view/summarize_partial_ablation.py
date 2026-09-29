"""All completed pairs; descriptive warning gains are not target-recall proof."""
import json
from pathlib import Path
import watch_final

ROOT=Path(__file__).resolve().parent
if __name__=='__main__':
    snapshot=watch_final.snapshot();rows=[]
    for row in snapshot['rows']:
        if not row['complete_pair']:continue
        nv,nf=row['native']['retained_alerts'];cv,cf=row['no_internal']['retained_alerts']
        if nv>0 and cv==0:outcome='warning_recovery';direction='positive'
        elif nv>0 and cv>0 and nf<cf:outcome='fixed_warning_reduction_retaining_version_hit';direction='positive'
        elif cv>0 and nv==0:outcome='lost_version_hit';direction='negative'
        elif nv>0 and cv>0 and nf>cf:outcome='more_fixed_warnings';direction='negative'
        else:outcome='no_primary_gain';direction='tie_or_other'
        rows.append({'case_id':row['sample_id'],'direction':direction,'outcome':outcome,
                     'native':[nv,nf],'no_internal':[cv,cf],
                     'native_candidate_sha256':row['native']['candidate_sha256'],
                     'control_candidate_sha256':row['no_internal']['candidate_sha256'],
                     'native_path':row['native']['path'],'control_path':row['no_internal']['path']})
    result={'checked_at':snapshot['checked_at'],'completed_pairs':len(rows),'planned_pairs':39,
            'positive_warning_changes':sum(x['direction']=='positive' for x in rows),
            'negative_warning_changes':sum(x['direction']=='negative' for x in rows),'rows':rows,
            'boundary':'All completed pairs, no positive selection. Counts are retained version-level warning observations, not target-level recall, population generalization, statistical significance or qualified mechanism gains.'}
    tmp=ROOT/'PARTIAL_ABLATION_COUNTS.tmp';tmp.write_text(json.dumps(result,indent=2))
    tmp.replace(ROOT/'PARTIAL_ABLATION_COUNTS.json')
    print(json.dumps({k:v for k,v in result.items() if k!='rows'}))
