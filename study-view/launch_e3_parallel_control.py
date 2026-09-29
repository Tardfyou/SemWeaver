"""Run original24 repeated control cells on an independent hot kernel lane."""
import ast
import fcntl
import importlib.util
from pathlib import Path
from resume_verified_queues import SkipExisting

ROOT=Path(__file__).resolve().parent
if __name__=='__main__':
    script=ROOT/'launch_e3_repeats.py'
    spec=importlib.util.spec_from_file_location('original_repeated_launcher',script)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    freeze=module.read(ROOT/'e3-selected-repeats/MATRIX_FREEZE.json')
    common=freeze['input_hashes'];assert all(module.sha(p)==h for p,h in common.items())
    lane=ROOT.parent/'LLM-Native/SemWeaver/artifacts/external/linux-matched-no_internal-v13'
    assert lane.is_dir()
    lock=(ROOT/'.e3-parallel-control-kernel.lock').open('a+')
    fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
    common={**common,str(Path(__file__)):module.sha(Path(__file__)),
            str(ROOT/'resume_verified_queues.py'):module.sha(ROOT/'resume_verified_queues.py')}
    module.save(ROOT/'e3-selected-repeats/PARALLEL_CONTROL_PLAN.json',{
        'planned_existing_control_cells':24,'additional_cells':0,'additional_response_allowance':0,
        'kernel_lane':str(lane),'input_hashes':common,'profile':freeze['profile'],
        'response_cap':32,'output_token_ceiling':16384,
        'boundary':'Original12, repeats2/3, control arm only; existing cells preserved, same algorithm/scorer/budgets.'})
    namespace=module.__dict__;namespace.update(output=ROOT/'e3-selected-repeats',common=common,
        deadline=freeze['deadline_epoch'],subjects=freeze['subjects'],profile=freeze['profile'],LANE=lane,lock=lock,
        preparation=module.read(ROOT/'E3_E4_PREPARATION.json'))
    tree=ast.parse(script.read_text())
    body=next(n.body for n in tree.body if isinstance(n,ast.If) and ast.unparse(n.test)=="__name__ == '__main__'")
    start=next(i for i,n in enumerate(body) if isinstance(n,ast.For))
    tail=ast.Module(body=body[start:],type_ignores=[])
    class Control(ast.NodeTransformer):
        def visit_For(self,node):
            if isinstance(node.target,ast.Name) and node.target.id=='arm':
                node.iter=ast.Tuple(elts=[ast.Constant('no_internal')],ctx=ast.Load())
            return self.generic_visit(node)
    tail=ast.fix_missing_locations(SkipExisting().visit(Control().visit(tail)))
    exec(compile(tail,str(script)+'[parallel-original-controls]','exec'),namespace)
