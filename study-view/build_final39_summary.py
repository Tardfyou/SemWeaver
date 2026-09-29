"""Build a full39-only summary; refuse partial or unbound effectiveness tables."""
import hashlib
import json
from pathlib import Path
import watch_final

ROOT=Path(__file__).resolve().parent
PROJECT=ROOT.parent
DATA=PROJECT/'LLM-Native/artifacts/fse_revision'
def read(path):return json.loads(Path(path).read_text())
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def metrics(rows):
    n=len(rows);tp=sum(x['vulnerable_alerts']>0 for x in rows);fp=sum(x['fixed_alerts']>0 for x in rows)
    return {'cases':n,'TP':tp,'FP':fp,'FN':n-tp,'TN':n-fp,
            'PDS':sum(x['vulnerable_alerts']>0 and x['fixed_alerts']==0 for x in rows),
            'precision':tp/(tp+fp) if tp+fp else 0,'recall':tp/n if n else 0,
            'F1':2*tp/(n+tp+fp) if n+tp+fp else 0,
            'fixed_warning_reports':sum(x['fixed_alerts'] for x in rows)}


def bound(row):
    assert sha(row['candidate'])==row['candidate_sha256']
    assert sha(row['origin'])==row['result_sha256']
    v=read(row['origin'])
    assert v['execution_valid'] and v['candidate_sha256']==row['candidate_sha256']
    assert not v.get('infrastructure_errors') and not v.get('scan_failure_artifacts')
    assert all(v[k]==row[k] for k in ('vulnerable_alerts','fixed_alerts'))
    return row


if __name__=='__main__':
    snapshot=watch_final.snapshot()
    assert snapshot['verified_pairs']==39, 'Full39 main comparison is not yet complete'
    freeze=read(ROOT/'MATRIX_FREEZE.json');cases=freeze['subjects']
    old_start={r['case_id']:r for r in read(DATA/'scanfix_v26_corrected_summary_v1/RESULT.json')['starting']['rows']}
    starts={}
    for case in cases:
        p=ROOT/'arm64-starting/RESULT.json' if case.startswith('G04_') else PROJECT/'native-trace-20260928/starting'/case/'RESULT.json'
        v=read(p);checker=DATA/'generate_only_materialized_v1/cases'/case/'csa/SAGenTestChecker.cpp'
        starts[case]=bound({'case_id':case,'candidate':str(checker),'candidate_sha256':sha(checker),
                    'vulnerable_alerts':v['vulnerable_alerts'],'fixed_alerts':v['fixed_alerts'],
                    'origin':str(p),'result_sha256':sha(p),'model_responses':0})
    portfolios={}
    for arm in ('native','no_internal'):
        rows=[]
        for pair in snapshot['rows']:
            path=Path(pair[arm]['path']);result=read(path/'RESULT.json');s=dict(result['selected'])
            if s['origin']=='starting_checker':s={**starts[pair['sample_id']],**{k:v for k,v in s.items() if k!='origin'}};s['origin']=starts[pair['sample_id']]['origin']
            s.update(case_id=pair['sample_id'],model_responses=result['state']['calls'],cell=str(path))
            rows.append(bound(s))
        portfolios[arm]={'rows':rows,'metrics':metrics(rows),'model_responses':sum(r['model_responses'] for r in rows)}
    no_report=read(ROOT/'knighter-no-report/RESULT.json');assert no_report['status']=='completed' and len(no_report['rows'])==24
    noops={r['case_id']:r for r in no_report['rows']}
    audit=read(ROOT/'BASELINE_NATURAL_REUSE_AUDIT.json');assert all(r['reuse_eligible'] for r in audit['rows'])
    old_batch=DATA/'generate_only_knighter_gpt6luna_high_v8/BATCH_RESULT.json'
    assert sha(old_batch)==audit['baseline_batch_sha256']
    old_results={x['case_id']:x['result'] for x in read(old_batch)['cases']}
    natural={r['case_id']:r for r in audit['rows']};baseline=[]
    for case in cases:
        if case in noops:
            n=noops[case];assert sha(n['summary_path'])==n['summary_sha256']
            row={**starts[case],'baseline_branch':'actual_zero_response_no_report','branch_record':str(ROOT/'knighter-no-report'/case/'NO_REPORT_RESULT.json')}
        elif case in natural:
            original=old_results[case]
            assert original['model']=='gpt-6-luna' and original['reasoning_effort']=='high' and original['model_calls_used']==1
            assert all(not r['refined'] and r['result']=='No-FP' for r in original['results'])
            row={**starts[case],'model_responses':1,'baseline_branch':'audited_natural_stop_reuse',
                 'actual_recorded_response_ceiling':natural[case]['recorded_response_ceiling']}
        else:
            cell=ROOT/'knighter'/case;execution=read(cell/'EXECUTION_MANIFEST.json');p=read(cell/'PAIRED_RESULT.json')
            assert execution['status']=='completed_baseline_pending_paired_replay' and execution['inputs_unchanged']
            assert p['status']=='completed' and p['model_responses']<=32
            assert sha(cell/'exchanges.health.json')==p['exchange_health_sha256']
            health=read(cell/'exchanges.health.json');assert health['status']=='completed' and not health['errors']
            assert health['consumed_model_responses']==p['model_responses']
            row={**p['selected'],'case_id':case,'model_responses':p['model_responses'],
                 'baseline_branch':'actual_recorded_refinement','cell':str(cell)}
        baseline.append(bound(row))
    portfolios['knighter']={'rows':baseline,'metrics':metrics(baseline),'model_responses':sum(r['model_responses'] for r in baseline)}
    precision={case for case,row in starts.items() if row['vulnerable_alerts']>0 and row['fixed_alerts']>0}
    assert len(precision)==15
    result={'status':'completed_main_baseline39','source_revision':freeze['source_revision'],
            'starting':{'rows':list(starts.values()),'metrics':metrics(list(starts.values()))},'portfolios':portfolios,
            'matched15':{arm:{'rows':[r for r in block['rows'] if r['case_id'] in precision],
                            'metrics':metrics([r for r in block['rows'] if r['case_id'] in precision])}
                         for arm,block in portfolios.items()},
            'builder_sha256':sha(Path(__file__)),
            'boundary':'All39 original subjects, corrected ARM64 scope, model-produced checkers and normal paired outcomes. Version-level warning metrics, not independently adjudicated target recall. Baseline no-report and naturally stopped branches explicit; repeated/model matrices are separate and must close before publication.'}
    output=ROOT/'FINAL39_SUMMARY.json'
    with output.open('x') as handle:json.dump(result,handle,indent=2)
    print(json.dumps({arm:block['metrics'] for arm,block in portfolios.items()}))
