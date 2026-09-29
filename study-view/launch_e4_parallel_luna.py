"""Run the already-planned Luna profile on an independent existing kernel lane.

No extra experimental cell or response allowance is introduced. The serial
queue skips existing directories, so all original cases/repeats remain one
cell per planned decode. GLM calls still use their single original lane.
"""
import ast
import fcntl
import importlib.util
import json
import time
from pathlib import Path
from resume_verified_queues import SkipExisting

ROOT=Path(__file__).resolve().parent

if __name__=='__main__':
    script=ROOT/'launch_e4_common64.py'
    spec=importlib.util.spec_from_file_location('frozen_e4_parallel_launcher',script)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    freeze=module.read(ROOT/'e4-common64/MATRIX_FREEZE.json')
    common=freeze['input_hashes'];assert all(module.sha(p)==h for p,h in common.items())
    lane=ROOT.parent/'LLM-Native/SemWeaver/artifacts/external/linux-e4-repeat-gpt-recovery-v10'
    assert lane.is_dir()
    lock=(ROOT/'.e4-parallel-luna-kernel.lock').open('a+')
    fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
    preparation=module.read(ROOT/'E3_E4_PREPARATION.json')
    profile=next(p for p in freeze['profiles'] if p['model']=='gpt-6-luna')
    jobs=[(profile,1,c) for c in freeze['subjects']]
    jobs += [(profile,r,c) for r in (2,3) for c in preparation['repeated_samples']]
    assert len(jobs)==63
    inputs={**common,str(Path(__file__)):module.sha(Path(__file__)),
            str(ROOT/'resume_verified_queues.py'):module.sha(ROOT/'resume_verified_queues.py')}
    module.save(ROOT/'e4-common64/PARALLEL_LUNA_PLAN.json',{
        'planned_existing_cells':63,'additional_cells':0,'additional_response_allowance':0,
        'kernel_lane':str(lane),'inputs':inputs,'profile':profile,
        'deadline_epoch':freeze['deadline_epoch'],'started_at':time.time(),
        'boundary':'Execution parallelism only; same frozen model/profile,2 responses,65536-output ceiling, selected method, original samples/repeats and scoring.'})
    namespace=module.__dict__
    namespace.update(output=ROOT/'e4-common64',common=inputs,deadline=freeze['deadline_epoch'],
                     preparation=preparation,jobs=jobs,LANE=lane)
    tree=ast.parse(script.read_text())
    body=next(n.body for n in tree.body if isinstance(n,ast.If) and ast.unparse(n.test)=="__name__ == '__main__'")
    # Keep the loop body, but the independent kernel lane owns its separate lock.
    start=next(i for i,n in enumerate(body) if isinstance(n,ast.For))
    tail=ast.fix_missing_locations(SkipExisting().visit(ast.Module(body=body[start:],type_ignores=[])))
    namespace['lock']=lock
    exec(compile(tail,str(script)+'[parallel-existing-Luna-cells]','exec'),namespace)
