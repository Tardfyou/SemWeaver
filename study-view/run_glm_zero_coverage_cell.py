"""Versioned mapped-high profile runner; no source/scoring/budget change."""
from pathlib import Path


if __name__ == '__main__':
    original = Path(__file__).with_name('run_glm_mapped_high_cell.py')
    source = original.read_text()
    old = "'glm_mapped_high_entry.py'"
    assert source.count(old) == 1
    source = source.replace(old, "'glm_mapped_high_zero_coverage_entry.py'")
    exec(compile(source, str(original)+'[zero-coverage-exit]', 'exec'),
         {'__name__': '__main__', '__file__': str(Path(__file__).resolve())})
