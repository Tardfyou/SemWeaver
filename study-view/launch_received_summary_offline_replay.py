"""Preserve the pre-dispatch blocked replay and repair only its offline gate."""
from pathlib import Path


if __name__ == '__main__':
    original = Path(__file__).with_name('launch_received_summary_replay.py')
    source = original.read_text()
    changes = {
        "output = ROOT / 'profile-repairs' / relative": "output = ROOT / 'profile-repairs' / relative / 'offline-no-dispatch'",
        "ROOT / 'run_received_summary_repair.py'": "ROOT / 'run_received_summary_offline_repair.py'",
        "ROOT / 'replay_received_summary_entry.py',":
        "ROOT / 'replay_received_summary_entry.py', ROOT / 'replay_received_summary_offline_entry.py', ROOT / 'run_received_summary_repair.py', original,",
    }
    for old, new in changes.items():
        expected = 2 if old == "ROOT / 'run_received_summary_repair.py'" else 1
        assert source.count(old) == expected
        source = source.replace(old, new)
    exec(compile(source, str(original) + '[no-dispatch-replay-repair]', 'exec'),
         {'__name__': '__main__', '__file__': str(Path(__file__).resolve()), 'original': original})
