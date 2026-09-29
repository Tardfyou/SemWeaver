"""Verify every planned E3/E4 cell, including designated operational repairs."""
import hashlib
import json
from pathlib import Path
import watch_final

ROOT=Path(__file__).resolve().parent
def read(path):
    try:return json.loads(Path(path).read_text())
    except (OSError,json.JSONDecodeError):return {}
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def inspect(parent,case,arm,profile,cap,ceiling,configuration_errors=()):
    repair=ROOT/'profile-repairs'/parent.relative_to(ROOT)
    cell=repair if (repair/'RUN_PLAN.json').exists() else parent
    schema_repair=repair/'feedback-schema-repair'
    if (schema_repair/'RUN_PLAN.json').exists():cell=schema_repair
    offline_repair=repair/'offline-no-dispatch'
    if (offline_repair/'RUN_PLAN.json').exists():cell=offline_repair
    basic=watch_final.integrity.inspect_cell(cell)
    errors=list(basic['integrity_errors'])+list(configuration_errors)
    result=read(cell/'RESULT.json');cfg=read(cell/'CONFIG.json');plan=read(cell/'RUN_PLAN.json')
    if basic['verified_complete']:
        if cfg.get('case_id')!=case or cfg.get('arm')!=arm or cfg.get('profile')!=profile:errors.append('condition_mismatch')
        if plan.get('model_response_cap')!=cap or plan.get('output_token_ceiling')!=ceiling:errors.append('budget_mismatch')
        calls=0
        for row in result.get('rows',[]):
            attempt=Path(row['attempt']);manifest=read(attempt/'RUN_MANIFEST.json')
            try:
                if sha(attempt/'RUN_MANIFEST.json')!=row['manifest_sha256']:errors.append('attempt_receipt_hash_mismatch')
                if sha(attempt/'SAGenTestChecker.cpp')!=manifest['candidate_sha256']:errors.append('attempt_checker_hash_mismatch')
            except (OSError,KeyError):errors.append('attempt_artifact_missing')
            if manifest.get('model')!=profile['model'] or manifest.get('reasoning_effort')!=profile['reasoning_effort']:errors.append('attempt_model_mismatch')
            calls+=manifest.get('model_calls_used',0)
            if row.get('paired_valid'):
                validation=read(attempt/'frozen_validation/RESULT.json')
                if not validation.get('execution_valid') or validation.get('infrastructure_errors') or validation.get('scan_failure_artifacts'):errors.append('invalid_scored_execution')
                if validation.get('candidate_sha256')!=manifest.get('candidate_sha256'):errors.append('validation_checker_mismatch')
                if any(validation.get(k)!=row.get(k) for k in ('vulnerable_alerts','fixed_alerts')):errors.append('row_counts_mismatch')
        if calls!=result.get('state',{}).get('calls') or calls>cap:errors.append('response_count_mismatch')
    verified=basic['verified_complete'] and not errors
    return {'case_id':case,'arm':arm,'model':profile['model'],'parent':str(parent),'selected_cell':str(cell),
            'status':basic['status'],'model_responses':basic['model_responses'],
            'verified_complete':verified,'integrity_errors':sorted(set(errors)),
            'retained_alerts':basic['retained_alerts'] if verified else [None,None],
            'checkpoint_alerts':basic['retained_alerts'] if not verified else None}


def select_profile(original_profile,e4,focused):
    model=original_profile['model'];errors=[]
    focus=ROOT/'e4-focused12'/model/'MATRIX_FREEZE.json'
    high=ROOT/'e4-explicit-high'/model/'MATRIX_FREEZE.json'
    if focused:
        freeze=read(focus)
        profile=freeze.get('profile',{'model':model,'reasoning_effort':'high'})
        folder=ROOT/'e4-focused12'/model
        if not freeze:errors.append('focused_configuration_missing')
        else:
            if freeze.get('subjects')!=e4['repeated_samples']:errors.append('focused_subjects_mismatch')
            if freeze.get('repeats')!=[1] or freeze.get('planned_cells')!=12:errors.append('focused_denominator_mismatch')
            if profile.get('model')!=model or profile.get('reasoning_effort')!='high':errors.append('focused_profile_mismatch')
            if freeze.get('response_cap')!=2 or freeze.get('output_token_ceiling')!=65536:errors.append('focused_budget_mismatch')
        # Never fall back to old GLM default/flat-only configurations.
    elif high.exists():
        profile=read(high)['profile'];folder=ROOT/'e4-explicit-high'/model
    else:
        profile=original_profile;folder=ROOT/'e4-common64'/model
    return profile,folder,errors


if __name__=='__main__':
    e3=read(ROOT/'e3-selected-repeats/MATRIX_FREEZE.json');e4=read(ROOT/'e4-common64/MATRIX_FREEZE.json')
    rows3=[inspect(ROOT/'e3-selected-repeats'/f'repeat-{repeat}'/arm/case,case,arm,e3['profile'],32,16384)
           for repeat in (2,3) for case in e3['subjects'] for arm in ('native','no_internal')]
    rows4=[]
    focused=(ROOT/'ROBUSTNESS_SCOPE_REDUCTION_20260929.json').exists()
    for original_profile in e4['profiles']:
        profile,folder,configuration_errors=select_profile(original_profile,e4,focused)
        for repeat in ((1,) if focused else (1,2,3)):
            for case in (e4['repeated_samples'] if focused else e4['subjects'] if repeat==1 else e4['repeated_samples']):
                parent=folder/f'repeat-{repeat}'/case
                if focused and profile['model']=='gpt-6-luna' and not parent.exists():
                    original=ROOT/'e4-common64'/profile['model']/f'repeat-{repeat}'/case
                    if original.exists():parent=original
                rows4.append(inspect(parent,case,'native',profile,2,65536,configuration_errors))
    assert len(rows3)==48 and len(rows4)==(36 if focused else 189)
    output={'e3':{'planned_new_cells':48,'verified_complete':sum(r['verified_complete'] for r in rows3),'rows':rows3},
            'e4':{'planned_cells':len(rows4),'verified_complete':sum(r['verified_complete'] for r in rows4),'rows':rows4},
            'boundary':'User-approved focused model sensitivity uses original12 and three models, one decode each. Main39/repeated ablation unchanged. Old outside-subset/default-effort records retained separately, not pooled. Pending/failures unscored.'}
    temporary=ROOT/'REMAINING_MATRIX_STATUS.tmp';temporary.write_text(json.dumps(output,indent=2))
    temporary.replace(ROOT/'REMAINING_MATRIX_STATUS.json')
    print(json.dumps({'e3_complete':output['e3']['verified_complete'],'e3_planned':48,
                      'e4_complete':output['e4']['verified_complete'],'e4_planned':len(rows4),
                      'integrity_errors':sum(bool(r['integrity_errors']) for r in rows3+rows4)}))
