"""Close only the full48 additional ablation and focused36 model matrix."""
import hashlib
import json
import subprocess
from pathlib import Path
import watch_final
from build_final39_summary import metrics,bound
from summarize_repeated_ablation import summarize

ROOT=Path(__file__).resolve().parent


def read(path):return json.loads(path.read_text())
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def require_complete(extra):
    assert extra['e3']['planned_new_cells']==48 and extra['e4']['planned_cells']==36
    assert extra['e3']['verified_complete']==48 and extra['e4']['verified_complete']==36,'Auxiliary experiment coverage incomplete'
    assert all(r['verified_complete'] and not r['integrity_errors']
               for group in ('e3','e4') for r in extra[group]['rows'])


def verify_glm_wire(path,model):
    requests=responses=0
    for line in path.read_text().splitlines():
        event=json.loads(line)
        if event['event']=='request':
            body=event['body'];requests+=1
            assert body['model']==model and body['max_tokens']==65536
            assert body.get('extra_body',{}).get('reasoning_effort')=='high'
            assert body.get('extra_body',{}).get('output_config',{}).get('effort')=='high'
        elif event['event']=='response':responses+=1
    assert requests>=responses==1,'Every processed attempt needs its received reply; do not infer missing replies'


if __name__=='__main__':
    subprocess.run(['python3',str(ROOT/'audit_remaining_matrix.py')],check=True,capture_output=True)
    status_path=ROOT/'REMAINING_MATRIX_STATUS.json';status_bytes=status_path.read_bytes()
    extra=json.loads(status_bytes);require_complete(extra)
    subjects=read(ROOT/'E3_E4_PREPARATION.json')['repeated_samples']
    repeated=summarize(subjects,watch_final.snapshot(),extra)
    assert repeated['status']=='complete' and repeated['complete_three_decode_subjects']==12
    blocks={}
    for model in ('gpt-6-luna','glm-5.3','glm-5.3-flash'):
        status_rows=[r for r in extra['e4']['rows'] if r['model']==model]
        assert len(status_rows)==12 and {r['case_id'] for r in status_rows}==set(subjects)
        rows=[]
        for status in status_rows:
            cell=Path(status['selected_cell']);result=read(cell/'RESULT.json');selected=dict(result['selected'])
            cfg=read(cell/'CONFIG.json');plan=read(cell/'RUN_PLAN.json')
            assert plan['model_response_cap']==2 and plan['output_token_ceiling']==65536
            assert cfg['profile']['model']==model and cfg['profile']['reasoning_effort']=='high'
            if model!='gpt-6-luna':
                for attempt in result['rows']:verify_glm_wire(Path(attempt['attempt'])/'provider_wire.jsonl',model)
            selected.update(case_id=status['case_id'],model_responses=result['state']['calls'],cell=str(cell))
            rows.append(bound(selected))
        blocks[model]={'profile':cfg['profile'],'rows':rows,'metrics':metrics(rows),
                       'model_responses':sum(r['model_responses'] for r in rows)}
    frozen_status=ROOT/'AUXILIARY_INPUT_STATUS.json'
    with frozen_status.open('xb') as handle:handle.write(status_bytes)
    helpers=('E3_E4_PREPARATION.json','MATRIX_FREEZE.json','summarize_repeated_ablation.py',
             'build_final39_summary.py','audit_remaining_matrix.py')
    output={'status':'completed_auxiliary_matrices','source_revision':read(ROOT/'MATRIX_FREEZE.json')['source_revision'],
            'repeated_ablation':repeated,'focused_model_sensitivity':blocks,
            'bindings':{str(frozen_status):hashlib.sha256(status_bytes).hexdigest(),
                        **{str(ROOT/name):sha(ROOT/name) for name in helpers},str(Path(__file__)):sha(Path(__file__))},
            'boundary':'Original12 repeated3 times under native/control; repeat1 actual main outputs. Three native model configurations, one decode each,2reply/65536 output ceilings. Models differ in actual computation; not cross-model evidence-versus-control causality, all39 robustness, target recall or unseen generalization. All weak/negative outcomes retained; partial or failed executions never filled as zeros.'}
    with (ROOT/'AUXILIARY_SUMMARY.json').open('x') as handle:json.dump(output,handle,indent=2)
    print(json.dumps({'status':output['status'],'repeated_subjects':12,'model_cells':36,
                      'models':{m:b['metrics'] for m,b in blocks.items()}}))
