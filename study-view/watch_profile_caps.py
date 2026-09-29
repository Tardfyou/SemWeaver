"""Observe exact terminal capped-probe failures, queue one independent repair."""
import fcntl
import json
import subprocess
import time
from pathlib import Path

ROOT=Path(__file__).resolve().parent
def read(path):
    try:return json.loads(path.read_text())
    except (OSError,json.JSONDecodeError):return {}

if __name__=='__main__':
    deadline=max(read(ROOT/g/'MATRIX_FREEZE.json')['deadline_epoch'] for g in ('e3-selected-repeats','e4-common64'))
    while time.time()<deadline:
        for group,pattern in [('e3-selected-repeats','repeat-*/*/*'),('e4-common64','*/repeat-*/*')]:
            for cell in sorted((ROOT/group).glob(pattern)):
                if read(cell/'RUN_MANIFEST.json').get('status')!='partial':continue
                if (ROOT/'profile-repairs'/cell.relative_to(ROOT)).exists():continue
                if read(cell/'CONFIG.json').get('architecture')!='x86':continue
                probes=[read(p) for p in cell.glob('continuation-*/checker_execution_probe/RESULT.json')]
                if not any(p.get('execution_health')=='completed' and p.get('diagnostic_parity') and
                           any(s.get('trace_capped') for s in p.get('scans',[])) for p in probes):continue
                log=ROOT/('cap-auto-'+str(cell.relative_to(ROOT)).replace('/','_')+'.log')
                if log.exists():continue
                lock=(ROOT/'.profile-cap-spare-kernel.lock').open('a+')
                try:fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
                except BlockingIOError:lock.close();continue
                lock.close()
                with log.open('x') as handle:
                    p=subprocess.Popen(['python3',str(ROOT/'launch_any_profile_cap_repair.py'),str(cell)],
                          stdout=handle,stderr=handle,start_new_session=True,stdin=subprocess.DEVNULL)
                print(json.dumps({'cap_repair_queued':str(cell),'pid':p.pid}),flush=True)
                while p.poll() is None and time.time()<deadline:time.sleep(min(60,max(1,deadline-time.time())))
        time.sleep(min(60,max(1,deadline-time.time())))
