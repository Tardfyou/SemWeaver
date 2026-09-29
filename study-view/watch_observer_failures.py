"""Queue the exact diagnosed observer failure; never blindly replay replies."""
import json
import subprocess
import time
from pathlib import Path

ROOT=Path(__file__).resolve().parent
def read(path):
    try:return json.loads(path.read_text())
    except (FileNotFoundError,json.JSONDecodeError):return {}

if __name__=='__main__':
    deadline=max(read(ROOT/g/'MATRIX_FREEZE.json')['deadline_epoch'] for g in ('e3-selected-repeats','e4-common64'))
    while time.time()<deadline:
        for group,pattern in [('runs','*-native'),('e3-selected-repeats','repeat-*/*/*'),('e4-common64','*/repeat-*/*')]:
            for cell in sorted((ROOT/group).glob(pattern)):
                if read(cell/'RUN_MANIFEST.json').get('status')!='partial':continue
                errors=[read(f).get('error') for f in cell.glob('continuation-*/checker_execution_probe/RESULT.json')]
                if 'Instrumented checker build failed' not in errors:continue
                logs=list(cell.glob('continuation-*/checker_execution_probe/trace_build/build_stdout_1.log'))
                if not any("no matching function for call to 'emit'" in p.read_text() and "to 'const clang::Stmt *'" in p.read_text() for p in logs):continue
                target=ROOT/'repairs'/cell.name if group=='runs' else ROOT/'profile-repairs'/cell.relative_to(ROOT)
                if target.exists():continue
                command=['python3',str(ROOT/('launch_observer_repair.py' if group=='runs' else 'launch_profile_observer_repair.py')),
                         cell.name if group=='runs' else str(cell)]
                log=ROOT/('observer-repair-'+str(cell.relative_to(ROOT)).replace('/','_')+'.log')
                if log.exists():continue
                with log.open('x') as handle:
                    p=subprocess.Popen(command,stdout=handle,stderr=handle,start_new_session=True,stdin=subprocess.DEVNULL)
                print(json.dumps({'observer_repair_queued':str(cell),'pid':p.pid}),flush=True)
        time.sleep(min(60,max(1,deadline-time.time())))
