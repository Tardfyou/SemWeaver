"""Throttle only our host launch queues; never cancel in-flight model calls."""
import json
import os
import signal
import time
from pathlib import Path

ROOT=Path(__file__).resolve().parent
QUEUE_NAMES={'resume_verified_queues.py','launch_e4_parallel_luna.py',
             'queue_contextless_repairs.py','watch_observer_failures.py',
             'launch_contextless_repair.py','launch_observer_repair.py',
             'launch_profile_observer_repair.py','launch_crash_checkpoint_repair.py'}
paused={}
def queues():
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit():continue
        try:
            argv=(proc/'cmdline').read_bytes().split(b'\0')
            scripts=[Path(v.decode()) for v in argv if v and v.endswith(b'.py')]
            if any(p.parent==ROOT and p.name in QUEUE_NAMES for p in scripts):
                yield int(proc.name),argv
        except (OSError,UnicodeDecodeError):continue

if __name__=='__main__':
    with (ROOT/'STORAGE_GUARD_POLICY.json').open('x') as handle:
        json.dump({'pause_launchers_below_GiB':64,'resume_above_GiB':96,
                   'scope':'Only named SemWeaver host launchers. Docker containers and model/validator children are not signalled.',
                   'boundary':'Operational disk safety, not an experimental stopping condition or goal pause. Existing model calls and raw-response recording continue.'},handle,indent=2)
    while True:
        info=os.statvfs(ROOT);free=info.f_bavail*info.f_frsize/2**30
        event=[]
        if free<64:
            for pid,argv in queues():
                if pid in paused:continue
                try:
                    os.kill(pid,signal.SIGSTOP);paused[pid]=argv;event.append({'launcher_stopped':pid})
                except ProcessLookupError:pass
        elif free>=96:
            for pid,argv in list(paused.items()):
                try:
                    current=(Path('/proc')/str(pid)/'cmdline').read_bytes().split(b'\0')
                    if current==argv:
                        os.kill(pid,signal.SIGCONT);event.append({'launcher_resumed':pid})
                except FileNotFoundError:pass
                del paused[pid]
        if event:
            with (ROOT/'STORAGE_GUARD_EVENTS.jsonl').open('a') as handle:
                handle.write(json.dumps({'timestamp':time.time(),'free_GiB':round(free,2),'actions':event})+'\n')
            print(json.dumps({'free_GiB':round(free,2),'actions':event}),flush=True)
        time.sleep(5)
