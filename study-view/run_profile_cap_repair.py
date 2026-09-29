"""Continue an audited profile prefix; no received response is resampled."""
import importlib.util
import subprocess
import sys
from pathlib import Path

ROOT=Path(__file__).resolve().parent
original=subprocess.run
def run(command,*args,**kwargs):
    if isinstance(command,list) and len(command)>1 and Path(command[1]).name in ('run_semweaver_treatment_case.py','provider_wire_entry.py'):
        command=[command[0],str(ROOT/'profile_cap_entry.py'),'--source-root',str(ROOT.parent/'LLM-Native/SemWeaver-v43'),
                 '--target-script',command[1],'--',*command[2:]]
    return original(command,*args,**kwargs)
subprocess.run=run
spec=importlib.util.spec_from_file_location('original_wire_with_capacity_repair',ROOT/'run_profile_wire.py')
wire=importlib.util.module_from_spec(spec);spec.loader.exec_module(wire)
profile=wire.profile


def restore_prefix(cfg,quality,cap):
    parent=Path(cfg['prefix_parent']);old=profile.read(parent/'CONFIG.json')
    plan=profile.read(parent/'RUN_PLAN.json')
    assert plan['model_response_cap']==cap
    assert all(profile.sha(p)==h for p,h in plan['inputs'].items())
    for key in ('case_id','arm','profile','architecture','objective','starting_result','evidence_bundle','fixed_reports_dir'):
        assert cfg[key]==old[key], 'Prefix protocol/config mismatch: '+key
    progress=profile.read(parent/'PROGRESS.json');rows=progress['rows']
    assert [r['attempt'] for r in rows]==cfg['prefix_attempts'] and rows
    calls=0
    for row in rows:
        attempt=Path(row['attempt']);manifest=profile.read(attempt/'RUN_MANIFEST.json')
        assert profile.sha(attempt/'RUN_MANIFEST.json')==row['manifest_sha256']
        assert manifest['model']==cfg['profile']['model'] and manifest['reasoning_effort']==cfg['profile']['reasoning_effort']
        assert manifest['model_calls_used']==row['calls']==1
        assert profile.sha(attempt/'SAGenTestChecker.cpp')==manifest['candidate_sha256']
        calls+=1
        if row['paired_valid']:
            v=profile.read(attempt/'frozen_validation/RESULT.json')
            assert v['execution_valid'] and not v.get('infrastructure_errors') and not v.get('scan_failure_artifacts')
            assert v['candidate_sha256']==manifest['candidate_sha256']
            assert all(v[k]==row[k] for k in ('vulnerable_alerts','fixed_alerts'))
        else:assert not manifest['success']
    assert calls==cfg['prefix_calls']==progress['state']['calls']<cap
    assert progress['state']==rows[-1]['state_after']
    best=progress['best']
    assert profile.sha(best['candidate'])==best['candidate_sha256']
    assert profile.sha(best['origin'])==best['result_sha256']
    required=quality(Path(best['candidate'])) if cfg['arm']=='native' else set()
    return dict(progress['state']),dict(best),list(rows),Path(rows[-1]['attempt']),required


def patched_source():
    source=(ROOT/'run_profile_cell.py').read_text()
    old="assert not cfg.get('prefix_attempts'), 'Profile first runs do not silently reuse unmatched prefixes'"
    assert source.count(old)==1
    source=source.replace(old,"assert cfg.get('prefix_parent') and cfg.get('prefix_attempts'), 'Audited prefix required'")
    old="    save(root / 'INITIAL_STATE.json', {'state': state, 'best': best})"
    assert source.count(old)==1
    source=source.replace(old,"    state,best,rows,latest,required = restore_prefix(cfg,quality,cap)\n"+old)
    return source


if __name__=='__main__':
    namespace={**profile.__dict__,'__name__':'audited_profile_continuation','restore_prefix':restore_prefix}
    exec(compile(patched_source(),str(ROOT/'run_profile_cell.py')+'[audited-prefix]','exec'),namespace)
    namespace['cell'](Path(sys.argv[1]))
