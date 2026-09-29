"""Versioned runner for offline replay, not a new provider dispatch."""
from pathlib import Path


if __name__ == '__main__':
    original = Path(__file__).with_name('run_received_summary_repair.py')
    source = original.read_text()
    old = "'replay_received_summary_entry.py'"
    assert source.count(old) == 1
    source = source.replace(old, "'replay_received_summary_offline_entry.py'")
    exec(compile(source, str(original) + '[offline-dispatch-reservation]', 'exec'),
         {'__name__': '__main__', '__file__': str(Path(__file__).resolve())})
