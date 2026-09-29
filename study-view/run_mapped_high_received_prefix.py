"""Resume one charged invalid-format prefix with the original mapped-high wire."""
import ast
import sys
from pathlib import Path
import run_glm_mapped_high_cell as mapped

ROOT=Path(__file__).resolve().parent

if __name__=='__main__':
    helper=ROOT/'run_profile_cap_repair.py';tree=ast.parse(helper.read_text())
    functions=[n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name in ('restore_prefix','patched_source')]
    assert len(functions)==2
    namespace={'ROOT':ROOT,'Path':Path,'profile':mapped.wire.profile}
    code=ast.unparse(ast.Module(body=functions,type_ignores=[]))
    assert code.count("parent / 'PROGRESS.json'")==1
    code=code.replace("parent / 'PROGRESS.json'","parent / 'PREFIX_PROGRESS.json'")
    exec(compile(code,str(helper)+'[received-format-prefix-only]','exec'),namespace)
    runner={**mapped.wire.profile.__dict__,'__name__':'mapped_high_received_prefix',
            'restore_prefix':namespace['restore_prefix']}
    exec(compile(namespace['patched_source'](),str(ROOT/'run_profile_cell.py')+'[charged-prefix]', 'exec'),runner)
    runner['cell'](Path(sys.argv[1]))
