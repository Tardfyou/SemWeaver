"""Same mapped high request with a corrected zero-coverage observer exit."""
from pathlib import Path


if __name__ == '__main__':
    original = Path(__file__).with_name('glm_high_pipeline_entry.py')
    source = original.read_text()
    changes = {
        "request['extra_body'] = {'reasoning_effort': 'high'}":
        "request['extra_body'] = {'reasoning_effort': 'high', 'output_config': {'effort': 'high'}}",
        "'adaptive_trace_probe.py'": "'adaptive_trace_probe_zero_coverage.py'",
    }
    for old, new in changes.items():
        assert source.count(old) == 1
        source = source.replace(old, new)
    exec(compile(source, str(original)+'[mapped-high-zero-coverage]', 'exec'),
         {'__name__': '__main__', '__file__': str(Path(__file__).resolve())})
