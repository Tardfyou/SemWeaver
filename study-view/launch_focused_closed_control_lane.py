"""One original focused cell on the closed control/replay clone.

The original queue skips allocated directories. No additional subject, reply
budget, experiment condition or outcome-based scheduling is introduced.
"""
from pathlib import Path


if __name__ == '__main__':
    root = Path(__file__).resolve().parent
    original = root / 'launch_focused_spare_cell.py'
    source = original.read_text()
    changes = {
        'linux-knighter-final-20260928': 'linux-knighter-replay-20260928',
        '.knighter-kernel.lock': '.e3-repeat3-control-replay-lane.lock',
        '.e3-repeat3-native-kernel.lock': '.e3-next-native-closed-control.lock',
        'FOCUSED_SPARE_{model}_{case[:3]}_PLAN.json': 'FOCUSED_CONTROL_LANE_{model}_{case[:3]}_PLAN.json',
        "str(Path(__file__)): module.sha(Path(__file__))":
        "str(original): module.sha(original), str(Path(__file__)): module.sha(Path(__file__))",
        "'semweaver-focused-spare-'": "'semweaver-focused-control-lane-'",
    }
    for old, new in changes.items():
        assert source.count(old) == 1, 'Original spare scheduler drift'
        source = source.replace(old, new)
    exec(compile(source, str(original) + '[one-closed-control-lane-cell]', 'exec'),
         {'__name__': '__main__', '__file__': str(Path(__file__).resolve()), 'original': original})
