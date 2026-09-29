"""Original repeat3 control cells on the independently closed replay lane.

Only scheduler routing changes; scored source, subjects and budgets stay frozen.
Both this adapter and the scheduler it derives from are recorded as inputs.
"""
from pathlib import Path

ROOT=Path(__file__).resolve().parent

if __name__=='__main__':
    original=ROOT/'launch_e3_parallel_repeat3_native.py'
    source=original.read_text()
    changes={
        "linux-knighter-final-20260928":"linux-knighter-replay-20260928",
        ".knighter-kernel.lock":".e3-repeat3-control-replay-lane.lock",
        ".e3-repeat3-native-kernel.lock":".e3-repeat3-control-kernel.lock",
        "PARALLEL_REPEAT3_NATIVE_PLAN.json":"PARALLEL_REPEAT3_CONTROL_PLAN.json",
        "'repeat':3,'arm':'native'":"'repeat':3,'arm':'no_internal'",
        "ast.Constant('native' if node.target.id=='arm' else 3)":"ast.Constant('no_internal' if node.target.id=='arm' else 3)",
        "common={**common,str(Path(__file__)):module.sha(Path(__file__)),":
        "common={**common,str(ROOT/'launch_e3_parallel_repeat3_native.py'):module.sha(ROOT/'launch_e3_parallel_repeat3_native.py'),str(Path(__file__)):module.sha(Path(__file__)),",
    }
    for old,new in changes.items():
        assert source.count(old)==1, 'Original scheduler drift'
        source=source.replace(old,new)
    source=source.replace('[parallel-original-repeat3-native]','[parallel-original-repeat3-control]')
    source=source.replace('repeat3 native only','repeat3 control only')
    exec(compile(source,str(original)+'[control-lane-only]', 'exec'),
         {'__name__':'__main__','__file__':str(Path(__file__).resolve())})
