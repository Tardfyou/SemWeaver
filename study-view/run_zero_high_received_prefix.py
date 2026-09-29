"""Charged-prefix runner using the mapped-high zero-coverage observer fix."""
from pathlib import Path
from types import SimpleNamespace


if __name__ == '__main__':
    root = Path(__file__).resolve().parent
    original = root / 'run_glm_mapped_high_cell.py'
    source = original.read_text()
    old = "'glm_mapped_high_entry.py'"
    assert source.count(old) == 1
    source = source.replace(old, "'glm_mapped_high_zero_coverage_entry.py'")
    namespace = {'__name__': 'zero_coverage_mapped_module', '__file__': str(original)}
    exec(compile(source, str(original)+'[zero-coverage-prefix-module]', 'exec'), namespace)
    mapped = SimpleNamespace(**namespace)
    prefix = root / 'run_mapped_high_received_prefix.py'
    source = prefix.read_text()
    old = 'import run_glm_mapped_high_cell as mapped'
    assert source.count(old) == 1
    source = source.replace(old, '# mapped runner installed from the frozen module above')
    exec(compile(source, str(prefix)+'[zero-coverage-received-prefix]', 'exec'),
         {'__name__': '__main__', '__file__': str(Path(__file__).resolve()), 'mapped': mapped})
