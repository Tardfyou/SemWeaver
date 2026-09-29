"""One more earliest absent planned repeat3 native cell, after prior closure."""
from pathlib import Path

if __name__=='__main__':
    original=Path(__file__).with_name('launch_next_native_on_closed_control_lane.py')
    source=original.read_text()
    old="'NEXT_NATIVE_CLOSED_CONTROL_PLAN.json'"
    assert source.count(old)==1
    source=source.replace(old,"'NEXT_NATIVE_CLOSED_CONTROL_PLAN_V2.json'")
    # Extend the replacement value, not the original template before its
    # count-checked transformations. The first failed startup made no cell.
    old="str(ROOT/'launch_e3_parallel_repeat3_native.py'):module.sha(ROOT/'launch_e3_parallel_repeat3_native.py'),str(Path(__file__))"
    new="str(ROOT/'launch_e3_parallel_repeat3_native.py'):module.sha(ROOT/'launch_e3_parallel_repeat3_native.py'),str(ROOT/'launch_next_native_on_closed_control_lane.py'):module.sha(ROOT/'launch_next_native_on_closed_control_lane.py'),str(Path(__file__))"
    assert source.count(old)==1
    source=source.replace(old,new)
    exec(compile(source,str(original)+'[second-single-cell]', 'exec'),
         {'__name__':'__main__','__file__':str(Path(__file__).resolve())})
