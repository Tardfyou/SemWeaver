"""User-approved fixed12 model sensitivity check; no outcome-based selection."""
import ast
import fcntl
import importlib.util
import subprocess
import sys
import time
from pathlib import Path
from resume_verified_queues import SkipExisting

ROOT=Path(__file__).resolve().parent
model=sys.argv[1];assert model in ('gpt-6-luna','glm-5.3','glm-5.3-flash')
script=ROOT/'launch_e4_common64.py'
spec=importlib.util.spec_from_file_location('original_focused_launcher',script)
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
old=module.read(ROOT/'e4-common64/MATRIX_FREEZE.json')
preparation=module.read(ROOT/'E3_E4_PREPARATION.json');subjects=preparation['repeated_samples'];assert len(subjects)==12
profile=dict(next(p for p in old['profiles'] if p['model']==model));profile['reasoning_effort']='high'
folder=ROOT/'e4-focused12'/model;folder.mkdir(parents=True,exist_ok=False)
lane_name={'gpt-6-luna':'linux-e4-repeat-gpt-v10','glm-5.3':'linux-e4-glm53-v11-clean','glm-5.3-flash':'linux-e4-final-20260928'}[model]
lane=ROOT.parent/'LLM-Native/SemWeaver/artifacts/external'/lane_name
assert lane.is_dir()
paths=[Path(__file__),ROOT/'glm_mapped_high_entry.py',ROOT/'glm_high_pipeline_entry.py',ROOT/'run_glm_mapped_high_cell.py',
       ROOT/'adaptive_trace_probe.py',ROOT/'fixed_trace_probe.py',ROOT/'ROBUSTNESS_SCOPE_REDUCTION_20260929.json',
       ROOT/'GLM_ANTHROPIC_EFFORT_MAPPING_SMOKE.json']
common={**old['input_hashes'],**{str(p):module.sha(p) for p in paths}}
assert all(module.sha(p)==h for p,h in common.items())
deadline=old['deadline_epoch']
jobs=[(profile,1,c) for c in subjects]
if model=='gpt-6-luna':
    # Same exact old high profile; complete or in-flight selected cells will be
    # independently audited/recovered, not resampled on a new nominal budget.
    jobs=[j for j in jobs if not (ROOT/'e4-common64'/model/'repeat-1'/j[2]).exists()]
module.save(folder/'MATRIX_FREEZE.json',{'task_version':'focused-model-sensitivity-v1','profile':profile,
    'subjects':subjects,'repeats':[1],'planned_cells':12,'newly_absent_cells':len(jobs),
    'source_revision':old['source_revision'],'response_cap':2,'output_token_ceiling':65536,
    'deadline_epoch':deadline,'input_hashes':common,'user_authority':'Latest request to appropriately reduce auxiliary experiments',
    'boundary':'Original12 fixed before current results; three models, one decode each. Main39 and repeated ablation unchanged. No full39/unseen-project/model-universal robustness claim. Old outside-subset runs retained separately.'})
namespace=module.__dict__;namespace.update(output=ROOT/'e4-focused12',common=common,deadline=deadline,
    preparation=preparation,jobs=jobs,LANE=lane)
tree=ast.parse(script.read_text())
body=next(n.body for n in tree.body if isinstance(n,ast.If) and ast.unparse(n.test)=="__name__ == '__main__'")
start=next(i for i,n in enumerate(body) if isinstance(n,ast.For))
tail=ast.fix_missing_locations(SkipExisting().visit(ast.Module(body=body[start:],type_ignores=[])))
original_run=subprocess.run
def run(command,*args,**kwargs):
    if isinstance(command,list) and model!='gpt-6-luna' and str(ROOT/'run_profile_wire.py') in command:
        command=list(command);command[command.index(str(ROOT/'run_profile_wire.py'))]=str(ROOT/'run_glm_mapped_high_cell.py')
    if isinstance(command,list) and '--name' in command:
        command=list(command);index=command.index('--name')+1;command[index]=command[index].replace('semweaver-e4-common-','semweaver-focused-')
    return original_run(command,*args,**kwargs)
subprocess.run=run
lock=(ROOT/('.focused-kernel-'+model+'.lock')).open('a+')
fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB);namespace['lock']=lock
exec(compile(tail,str(script)+'[focused-original12]','exec'),namespace)
