"""Independent full-cohort baseline provenance and normal-execution audit."""
import json
from pathlib import Path
from build_final39_summary import ROOT,PROJECT,DATA,read,sha,bound,metrics

if __name__=='__main__':
    subjects=read(ROOT/'MATRIX_FREEZE.json')['subjects']
    no_report=read(ROOT/'knighter-no-report/RESULT.json');assert no_report['status']=='completed'
    noops={r['case_id']:r for r in no_report['rows']};assert len(noops)==24
    audit=read(ROOT/'BASELINE_NATURAL_REUSE_AUDIT.json')
    batch_path=DATA/'generate_only_knighter_gpt6luna_high_v8/BATCH_RESULT.json'
    assert sha(batch_path)==audit['baseline_batch_sha256']
    original={x['case_id']:x['result'] for x in read(batch_path)['cases']}
    natural={r['case_id']:r for r in audit['rows']};assert len(natural)==2
    rows=[]
    for case in subjects:
        if case in noops:
            r=noops[case];summary=read(r['summary_path'])
            assert sha(r['summary_path'])==r['summary_sha256']
            assert summary['no_fixed_reports'] and summary['model_calls_used']==0 and summary['execution_valid']
            health=read(ROOT/'knighter-no-report'/case/'exchanges.health.json')
            assert health['status']=='completed' and health['consumed_model_responses']==0 and not health['errors']
            row={**r,'branch':'actual_no_report','model_responses':0}
        elif case in natural:
            r=natural[case];record=original[case]
            assert r['reuse_eligible'] and r['exact_prompt_equivalence']
            assert record['model']=='gpt-6-luna' and record['reasoning_effort']=='high' and record['model_calls_used']==1
            assert not record['model_call_budget_exhausted']
            assert all(not a['refined'] and a['result']=='No-FP' for a in record['results'])
            history=batch_path.parent/record['checker_id']/'prompt_history/0'
            report=record['fixed_report_ids'][0]
            assert sha(history/f'check_report-{report}.md')==r['prompt_sha256']
            assert sha(history/f'response_check_report-{report}.md')==r['response_sha256']
            start_path=PROJECT/'native-trace-20260928/starting'/case/'RESULT.json';start=read(start_path)
            candidate=DATA/'generate_only_materialized_v1/cases'/case/'csa/SAGenTestChecker.cpp'
            row={'case_id':case,'candidate':str(candidate),'candidate_sha256':sha(candidate),
                 'vulnerable_alerts':start['vulnerable_alerts'],'fixed_alerts':start['fixed_alerts'],
                 'origin':str(start_path),'result_sha256':sha(start_path),'branch':'audited_natural_stop',
                 'model_responses':1,'original_ceiling':record['max_model_calls']}
        else:
            cell=ROOT/'knighter'/case;execution=read(cell/'EXECUTION_MANIFEST.json')
            plan=read(cell/'RUN_PLAN.json');assert all(sha(p)==h for p,h in plan['inputs'].items())
            pair=read(cell/'PAIRED_RESULT.json');health=read(cell/'exchanges.health.json')
            assert execution['status']=='completed_baseline_pending_paired_replay' and execution['inputs_unchanged']
            assert pair['status']=='completed' and health['status']=='completed' and not health['errors']
            assert sha(cell/'exchanges.health.json')==pair['exchange_health_sha256']
            assert health['consumed_model_responses']==pair['model_responses']<=32
            assert sha(cell/'exchanges.jsonl')==health['exchange_log_sha256']
            summaries=list((cell/'output').glob('*/MATCHED_BASELINE_RESULT.json'));assert len(summaries)==1
            summary=read(summaries[0]);assert sha(summaries[0])==pair['baseline_summary_sha256']
            assert all(summary[k]==v for k,v in {'model':'gpt-6-luna','reasoning_effort':'high','max_model_calls':32,'max_tokens':16384,'max_tries':2,'max_fp_reports':5}.items())
            row={**pair['selected'],'case_id':case,'branch':'actual_recorded_refinement',
                 'model_responses':pair['model_responses'],'pair_record':str(cell/'PAIRED_RESULT.json')}
        rows.append(bound(row))
    assert len(rows)==39 and {r['case_id'] for r in rows}==set(subjects)
    assert sum(r['branch']=='actual_recorded_refinement' for r in rows)==13
    output={'status':'verified_baseline39','rows':rows,'metrics':metrics(rows),
            'model_responses':sum(r['model_responses'] for r in rows),'auditor_sha256':sha(Path(__file__)),
            'boundary':'39 actual/equivalence-audited branches. No-report24 and natural-stop2 explicitly distinguished from13 fresh32-ceiling executions. Corrected paired scopes and all selected checker/output hashes verified. Warning-version metrics, not target-specific recall.'}
    with (ROOT/'BASELINE39_CLOSURE.json').open('x') as h:json.dump(output,h,indent=2)
    print(json.dumps({'status':output['status'],'cases':39,'fresh_cases':13,'actual_no_report':24,'natural_reuse':2,
                      'model_responses':output['model_responses'],'metrics':output['metrics']}))
