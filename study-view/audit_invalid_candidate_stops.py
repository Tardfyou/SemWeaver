"""Read original model/compiler diagnostics behind completed ineffective stops."""
import hashlib
import json
from collections import Counter
from pathlib import Path

ROOT=Path(__file__).resolve().parent

if __name__=='__main__':
    status=json.loads((ROOT/'REMAINING_MATRIX_STATUS.json').read_text());rows=[];counts=Counter()
    for cell_row in status['e3']['rows']:
        if not cell_row['verified_complete']:continue
        cell=Path(cell_row['selected_cell']);result=json.loads((cell/'RESULT.json').read_text())
        if result['stop_reason']!='three_model_attempts_without_valid_candidate':continue
        attempts=[]
        for row in result['rows']:
            if row['paired_valid']:continue
            path=Path(row['attempt'])/'RUN_MANIFEST.json';raw=path.read_bytes();m=json.loads(raw)
            text=str(m.get('latest_failure_text','') or '')
            message=str(m.get('error_message','') or '')
            if m.get('failure_type')=='compile_failure' and 'error:' in text:
                category='generated_candidate_compile_error'
            elif m.get('latest_failure_title')=='apply_patch.exact_edits' and 'old_snippet' in text:
                category='model_edit_contract_error'
            elif ('最终工件与基线相同' in message or '没有提供任何有效 edits' in message):
                category='model_no_effective_edit'
            else:category='unclassified_requires_inspection'
            markers=[s for s in ('No space left on device','Cannot load plugin','Unable to load plugin',
                                  'Connection error','APITimeoutError','gateway_concurrency_limit') if s in text+message]
            if markers:category='possible_infrastructure_or_provider_problem'
            counts[category]+=1
            attempts.append({'attempt':row['attempt'],'manifest_sha256':hashlib.sha256(raw).hexdigest(),
                'classification':category,'failure_type':m.get('failure_type'),
                'failure_title':m.get('latest_failure_title'),'infrastructure_markers':markers,
                'compiler_error_lines':[line for line in text.splitlines() if 'error:' in line],
                'original_error_message':message})
        rows.append({'case_id':cell_row['case_id'],'arm':cell_row['arm'],'cell':str(cell),
                     'actual_model_responses':result['state']['calls'],'attempts':attempts})
    output={'status':'diagnostic_record_audit','stopped_cells':len(rows),'attempt_classifications':dict(counts),
            'additional_model_calls':0,'rows':rows,
            'boundary':'Original diagnostics inspected, not fresh executions or a proof of all environment behavior. Compile/API mistakes and no edits are candidate failures, not false-negative scans. Selected scored checkers still require their independently verified normal paired results. Unclassified/infrastructure markers require inspection, not zero scoring or silent resampling.'}
    temporary=ROOT/'INVALID_CANDIDATE_STOP_AUDIT.tmp';temporary.write_text(json.dumps(output,indent=2))
    temporary.replace(ROOT/'INVALID_CANDIDATE_STOP_AUDIT.json')
    print(json.dumps({k:v for k,v in output.items() if k not in ('rows','boundary')}))
