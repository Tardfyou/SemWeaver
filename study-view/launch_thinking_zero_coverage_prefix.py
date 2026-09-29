"""Thinking-only first reply recovery on a closed lane with correct zero coverage."""
from pathlib import Path


if __name__ == '__main__':
    original = Path(__file__).with_name('launch_thinking_only_prefix_repair.py')
    source = original.read_text()
    marker = '    exec(compile(source,'
    assert source.count(marker) == 1
    insertion = '''    further = {
        'run_mapped_high_received_prefix.py': 'run_zero_high_received_prefix.py',
        'linux-state-recovery-v41': 'linux-knighter-final-20260928',
        '.profile-cap-spare-kernel.lock': '.knighter-kernel.lock',
    }
    for old,new in further.items():
        assert source.count(old)==(2 if old=='run_mapped_high_received_prefix.py' else 1)
        source=source.replace(old,new)
    old="paths=[Path(__file__),ROOT/'launch_received_format_prefix.py'"
    new="paths=[Path(__file__),ROOT/'launch_thinking_only_prefix_repair.py',ROOT/'run_mapped_high_received_prefix.py',ROOT/'glm_mapped_high_zero_coverage_entry.py',ROOT/'adaptive_trace_probe_zero_coverage.py',ROOT/'launch_received_format_prefix.py'"
    assert source.count(old)==1
    source=source.replace(old,new)
'''
    source = source.replace(marker, insertion + marker)
    exec(compile(source, str(original)+'[zero-coverage-and-closed-lane]', 'exec'),
         {'__name__': '__main__', '__file__': str(Path(__file__).resolve())})
