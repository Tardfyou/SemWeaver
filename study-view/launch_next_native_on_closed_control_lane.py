"""Run one earliest absent original repeat3 native cell on a closed control lane.

Scheduling only: no new cell, decoder budget, source, scoring or selection rule.
"""
from pathlib import Path

ROOT=Path(__file__).resolve().parent

if __name__=='__main__':
    original=ROOT/'launch_e3_parallel_repeat3_native.py';source=original.read_text()
    changes={
        'linux-knighter-final-20260928':'linux-knighter-replay-20260928',
        '.knighter-kernel.lock':'.e3-repeat3-control-replay-lane.lock',
        '.e3-repeat3-native-kernel.lock':'.e3-next-native-closed-control.lock',
        'PARALLEL_REPEAT3_NATIVE_PLAN.json':'NEXT_NATIVE_CLOSED_CONTROL_PLAN.json',
        "namespace=module.__dict__;namespace.update(":
        "pending=[c for c in freeze['subjects'] if not (ROOT/'e3-selected-repeats/repeat-3/native'/c).exists()]\n    assert pending, 'No absent native repeat3 cell'\n    freeze={**freeze,'subjects':pending[:1]}\n    namespace=module.__dict__;namespace.update(",
        "common={**common,str(Path(__file__)):module.sha(Path(__file__)),":
        "common={**common,str(ROOT/'launch_e3_parallel_repeat3_native.py'):module.sha(ROOT/'launch_e3_parallel_repeat3_native.py'),str(Path(__file__)):module.sha(Path(__file__)),",
        "'planned_existing_cells':12":"'planned_existing_cells':1",
    }
    for old,new in changes.items():
        assert source.count(old)==1,'Original native scheduler drift'
        source=source.replace(old,new)
    exec(compile(source,str(original)+'[one-absent-cell-closed-control-lane]','exec'),
         {'__name__':'__main__','__file__':str(Path(__file__).resolve())})
