"""Resume only absent cells from frozen launchers; preserve all existing data.

Execute the original launcher loop, not a rewritten experiment algorithm.
Completed results lacking the parent-process receipt are audited separately.
"""
import argparse
import ast
import importlib.util
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent


class SkipExisting(ast.NodeTransformer):
    def visit_Expr(self, node):
        self.generic_visit(node)
        value = node.value
        if (isinstance(value, ast.Call) and isinstance(value.func, ast.Attribute)
                and isinstance(value.func.value, ast.Name) and value.func.value.id=='cell'
                and value.func.attr=='mkdir'):
            return [*ast.parse("if cell.exists():\n    print(json.dumps({'preserved_existing_cell': str(cell)}), flush=True)\n    continue").body, node]
        return node


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('queue', choices=['main','e3','e4','baseline'])
    args = parser.parse_args()
    if args.queue=='main':
        path = ROOT/'resume_missing_cells.py'
        text = path.read_text().replace("'MISSING_CELL_RESUME_PLAN.json'", "'MISSING_CELL_RESUME_PLAN_RESTART_20260929.json'")
        text = text.replace("'MISSING_CELL_RESUME_RESULT.json'", "'MISSING_CELL_RESUME_RESULT_RESTART_20260929.json'")
        # The original scheduler already limits this to directories that do not exist.
        exec(compile(text,str(path),'exec'), {'__name__':'__main__','__file__':str(path)})
        return
    name, folder = {'e3':('launch_e3_repeats.py','e3-selected-repeats'),
                    'e4':('launch_e4_common64.py','e4-common64'),
                    'baseline':('launch_baseline.py','knighter')}[args.queue]
    path = ROOT/name
    spec = importlib.util.spec_from_file_location('frozen_launcher',path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    freeze = module.read(ROOT/folder/'MATRIX_FREEZE.json')
    common = freeze['input_hashes']
    assert all(module.sha(p)==h for p,h in common.items()), 'Frozen input drift'
    namespace = module.__dict__
    namespace.update(output=ROOT/folder, common=common, deadline=freeze['deadline_epoch'],
                     subjects=freeze['subjects'], preparation=module.read(ROOT/'E3_E4_PREPARATION.json'))
    if args.queue=='e3':
        namespace['profile']=freeze['profile']
    if args.queue=='e4':
        profiles=freeze['profiles']; preparation=namespace['preparation']
        namespace['jobs']=[(p,1,c) for p in profiles for c in freeze['subjects']]
        namespace['jobs'] += [(p,r,c) for p in profiles for r in (2,3) for c in preparation['repeated_samples']]
    tree=ast.parse(path.read_text())
    if args.queue=='baseline':
        body=next(n.body for n in tree.body if isinstance(n,ast.FunctionDef) and n.name=='main')
    else:
        body=next(n.body for n in tree.body if isinstance(n,ast.If) and ast.unparse(n.test)=="__name__ == '__main__'")
    start=next(i for i,n in enumerate(body) if isinstance(n,ast.Assign)
               and any(isinstance(t,ast.Name) and t.id=='lock' for t in n.targets))
    tail=ast.Module(body=body[start:],type_ignores=[])
    tail=ast.fix_missing_locations(SkipExisting().visit(tail))
    ledger=ROOT/folder/'QUEUE_RESUMPTION_20260929.json'
    module.save(ledger, {'queue':args.queue,'launcher_sha256':module.sha(path),
                'resume_script_sha256':module.sha(Path(__file__)), 'freeze_sha256':module.sha(ROOT/folder/'MATRIX_FREEZE.json'),
                'boundary':'Only absent directories launched; existing results, replies and failures are never overwritten. Original frozen model, budgets, commands, stopping and adoption retained.'})
    exec(compile(tail,str(path)+'[absent-only-resume]','exec'),namespace)


if __name__=='__main__':
    main()
