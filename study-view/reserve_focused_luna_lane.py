"""Reserve the existing slot2 lock while the focused Luna launcher uses its lane.

No running model, checker, configuration or frozen input is changed. The old
contextless-repair queue already respects this lock and can proceed afterwards.
"""
import fcntl
import json
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent


def launcher_live(pid):
    try:
        command = Path(f'/proc/{pid}/cmdline').read_bytes().split(b'\0')
    except FileNotFoundError:
        return False
    return (str(ROOT / 'launch_focused_robustness.py').encode() in command
            and b'gpt-6-luna' in command)


def focused_container_live():
    result = subprocess.run(
        ['docker', 'ps', '--filter', 'name=semweaver-focused-gpt-6-luna-',
         '--format', '{{.Names}}'], capture_output=True, text=True, check=True)
    return bool(result.stdout.strip())


if __name__ == '__main__':
    pid = int(sys.argv[1])
    assert launcher_live(pid), 'Verify the exact live launcher before reservation'
    with (ROOT.parent / 'native-trace-20260928/.slot-2.lock').open('a+') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        print(json.dumps({'event': 'lane_reserved', 'launcher_pid': pid,
                          'time': time.time()}), flush=True)
        while launcher_live(pid) or focused_container_live():
            time.sleep(30)
        print(json.dumps({'event': 'lane_released', 'time': time.time()}), flush=True)
