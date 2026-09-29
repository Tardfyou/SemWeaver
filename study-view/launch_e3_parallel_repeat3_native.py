"""Use the closed baseline lane for original repeat3 native cells only.

No new subjects, repeats, response allowance, selection or scoring rule.
"""
import ast
import fcntl
import importlib.util
import json
import subprocess
from pathlib import Path
from resume_verified_queues import SkipExisting

ROOT=Path(__file__).resolve().parent

if __name__=='__main__':
    script=ROOT/'launch_e3_repeats.py'
    spec=importlib.util.spec_from_file_location('original_repeat3_launcher',script)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    freeze=module.read(ROOT/'e3-selected-repeats/MATRIX_FREEZE.json')
    baseline=module.read(ROOT/'BASELINE39_CLOSURE.json')
    assert baseline['status']=='verified_baseline39' and len(baseline['rows'])==39
    common=freeze['input_hashes'];assert all(module.sha(p)==h for p,h in common.items())
    lane=ROOT.parent/'LLM-Native/SemWeaver/artifacts/external/linux-knighter-final-20260928'
    assert lane.is_dir() and not lane.is_symlink()
    baseline_lock=(ROOT/'.knighter-kernel.lock').open('a+')
    fcntl.flock(baseline_lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
    live=subprocess.check_output(['docker','ps','-q','--filter','name=semweaver'],text=True).split()
    containers=json.loads(subprocess.check_output(['docker','inspect',*live],text=True)) if live else []
    assert not any(str(lane) in ' '.join((c['Config'].get('Cmd') or [])+(c['Config'].get('Env') or []))
                   for c in containers), 'Do not reuse a live kernel lane'
    lock=(ROOT/'.e3-repeat3-native-kernel.lock').open('a+')
    fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
    common={**common,str(Path(__file__)):module.sha(Path(__file__)),
            str(ROOT/'resume_verified_queues.py'):module.sha(ROOT/'resume_verified_queues.py')}
    module.save(ROOT/'e3-selected-repeats/PARALLEL_REPEAT3_NATIVE_PLAN.json',{
        'planned_existing_cells':12,'additional_cells':0,'additional_response_allowance':0,
        'repeat':3,'arm':'native','kernel_lane':str(lane),'input_hashes':common,
        'baseline_closure_sha256':module.sha(ROOT/'BASELINE39_CLOSURE.json'),
        'profile':freeze['profile'],'response_cap':32,'output_token_ceiling':16384,
        'boundary':'Original12, repeat3 native only; absent directories only, existing data preserved. Original loop/algorithm/scorer/budgets. Closed baseline kernel clone reused under its original lock.'})
    namespace=module.__dict__;namespace.update(output=ROOT/'e3-selected-repeats',common=common,
        deadline=freeze['deadline_epoch'],subjects=freeze['subjects'],profile=freeze['profile'],LANE=lane,lock=lock,
        preparation=module.read(ROOT/'E3_E4_PREPARATION.json'))
    tree=ast.parse(script.read_text())
    body=next(n.body for n in tree.body if isinstance(n,ast.If) and ast.unparse(n.test)=="__name__ == '__main__'")
    start=next(i for i,n in enumerate(body) if isinstance(n,ast.For))
    class Repeat3Native(ast.NodeTransformer):
        def visit_For(self,node):
            if isinstance(node.target,ast.Name) and node.target.id in ('arm','repeat'):
                node.iter=ast.Tuple(elts=[ast.Constant('native' if node.target.id=='arm' else 3)],ctx=ast.Load())
            return self.generic_visit(node)
    tail=ast.fix_missing_locations(SkipExisting().visit(Repeat3Native().visit(ast.Module(body=body[start:],type_ignores=[]))))
    exec(compile(tail,str(script)+'[parallel-original-repeat3-native]','exec'),namespace)
    baseline_lock.close()
