"""Periodically repair known pre-model observer failures on the reserved lane."""
import fcntl
import json
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PROJECT = ROOT.parent


def read(path):
    try:
        return json.loads(path.read_text())
    except (FileNotFoundError, json.JSONDecodeError):
        return {}


if __name__ == '__main__':
    deadline = read(ROOT / 'MATRIX_FREEZE.json')['deadline_epoch']
    while time.time() < deadline:
        pending = []
        for cell in sorted((ROOT / 'runs').glob('*-native')):
            if read(cell / 'RUN_MANIFEST.json').get('status') != 'partial':
                continue
            replacement = ROOT / 'repairs' / cell.name
            if replacement.exists():
                continue
            probes = list(cell.glob('continuation-*/checker_execution_probe/RESULT.json'))
            if any(read(path).get('error') == 'No supported CheckerContext-bearing method bodies' for path in probes):
                pending.append(cell.name)
        if pending:
            lock = (PROJECT / 'native-trace-20260928/.slot-2.lock').open('a+')
            free = True
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                free = False
            lock.close()
            if free:
                name = pending[0]
                log = ROOT / ('repair-queue-' + name + '.log')
                with log.open('x') as stream:
                    process = subprocess.Popen(['python3', str(ROOT / 'launch_contextless_repair.py'), name],
                                               stdout=stream, stderr=stream)
                    print(json.dumps({'repair_launched': name, 'pid': process.pid}), flush=True)
                    while process.poll() is None and time.time() < deadline:
                        time.sleep(min(60, max(1, deadline-time.time())))
                print(json.dumps({'repair_finished': name, 'return_code': process.poll()}), flush=True)
        # A terminal main summary is not required: disk exhaustion may have lost
        # that receipt. No unresolved cell is marked complete by this watcher.
        time.sleep(min(60, max(1, deadline-time.time())))
