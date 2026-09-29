"""Fresh explicit-high GLM robustness; retain default-effort runs separately."""
import ast
import importlib.util
import json
import subprocess
import sys
import time
from pathlib import Path
from resume_verified_queues import SkipExisting

ROOT=Path(__file__).resolve().parent
model=sys.argv[1]
assert model in ('glm-5.3','glm-5.3-flash')
script=ROOT/'launch_e4_common64.py'
spec=importlib.util.spec_from_file_location('frozen_robustness_launcher',script)
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
freeze=module.read(ROOT/'e4-common64/MATRIX_FREEZE.json')
smoke=module.read(ROOT/'GLM_EXPLICIT_HIGH_SMOKE.json');assert all(r['status']=='pass' for r in smoke['rows'])
profile={**next(p for p in freeze['profiles'] if p['model']==model),'reasoning_effort':'high'}
output=ROOT/'e4-explicit-high'/model;output.mkdir(parents=True,exist_ok=False)
lane=ROOT.parent/'LLM-Native/SemWeaver/artifacts/external'/('linux-e4-glm53-anthropic-v17' if model=='glm-5.3' else 'linux-e4-flash-anthropic-v17')
assert lane.is_dir()
preparation=module.read(ROOT/'E3_E4_PREPARATION.json')
jobs=[(profile,1,c) for c in freeze['subjects']]+[(profile,r,c) for r in (2,3) for c in freeze['repeated_samples']]
assert len(jobs)==63
common=dict(freeze['input_hashes'])
for path in [Path(__file__),ROOT/'run_glm_high_cell.py',ROOT/'glm_high_pipeline_entry.py',
             ROOT/'adaptive_trace_probe.py',ROOT/'fixed_trace_probe.py',ROOT/'GLM_EXPLICIT_HIGH_SMOKE.json']:
    common[str(path)]=module.sha(path)
assert all(module.sha(p)==h for p,h in common.items())
deadline=freeze['deadline_epoch']
module.save(output/'MATRIX_FREEZE.json',{'profile':profile,'subjects':freeze['subjects'],
    'repeated_samples':freeze['repeated_samples'],'planned_cells':63,'response_cap':2,
    'output_token_ceiling':65536,'deadline_epoch':deadline,'input_hashes':common,
    'source_revision':freeze['source_revision'],'scope':'Fresh explicit-high profile; older default-effort observations remain separate. Luna original high cells are unchanged and reused only within their exact original protocol.'})
namespace=module.__dict__;namespace.update(output=ROOT/'e4-explicit-high',common=common,deadline=deadline,
    preparation=preparation,jobs=jobs,LANE=lane)
tree=ast.parse(script.read_text())
body=next(n.body for n in tree.body if isinstance(n,ast.If) and ast.unparse(n.test)=="__name__ == '__main__'")
start=next(i for i,n in enumerate(body) if isinstance(n,ast.For))
tail=ast.fix_missing_locations(SkipExisting().visit(ast.Module(body=body[start:],type_ignores=[])))
original_run=subprocess.run
def run(command,*args,**kwargs):
    if isinstance(command,list) and str(ROOT/'run_profile_wire.py') in command:
        command=list(command);command[command.index(str(ROOT/'run_profile_wire.py'))]=str(ROOT/'run_glm_high_cell.py')
    return original_run(command,*args,**kwargs)
subprocess.run=run
import fcntl
lock=(ROOT/('.explicit-high-'+model+'.lock')).open('a+')
fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB);namespace['lock']=lock
exec(compile(tail,str(script)+'[fresh-explicit-high-GLM-profile]','exec'),namespace)
