"""One original absent cell on the closed replay lane with corrected observer."""
from pathlib import Path


if __name__ == '__main__':
    original = Path(__file__).with_name('launch_focused_spare_cell.py')
    source = original.read_text()
    changes = {
        'linux-knighter-final-20260928': 'linux-knighter-replay-20260928',
        '.knighter-kernel.lock': '.e3-repeat3-control-replay-lane.lock',
        '.e3-repeat3-native-kernel.lock': '.e3-next-native-closed-control.lock',
        "str(ROOT / 'run_glm_mapped_high_cell.py')": "str(ROOT / 'run_glm_zero_coverage_cell.py')",
        "namespace.update(output=ROOT / 'e4-focused12'": "namespace.update(lock=kernel_lock, output=ROOT / 'e4-focused12'",
        'FOCUSED_SPARE_{model}_{case[:3]}_PLAN.json': 'FOCUSED_CONTROL_ZERO_{model}_{case[:3]}_PLAN.json',
        "'semweaver-focused-spare-'": "'semweaver-focused-control-zero-'",
        "str(Path(__file__)): module.sha(Path(__file__))":
        "str(original): module.sha(original), **{str(ROOT/n): module.sha(ROOT/n) for n in ('run_glm_zero_coverage_cell.py','glm_mapped_high_zero_coverage_entry.py','adaptive_trace_probe_zero_coverage.py')}, str(Path(__file__)): module.sha(Path(__file__))",
    }
    for old, new in changes.items():
        assert source.count(old) == 1
        source = source.replace(old, new)
    exec(compile(source, str(original)+'[closed-control-zero-coverage-aware]', 'exec'),
         {'__name__': '__main__', '__file__': str(Path(__file__).resolve()), 'original': original})
