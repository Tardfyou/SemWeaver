"""Periodic whole-study progress; a live process is checked, not assumed."""
import datetime
import json
import subprocess
import time
from pathlib import Path
import watch_final

ROOT=Path(__file__).resolve().parent
def read(path):
    try:return json.loads(path.read_text())
    except (OSError,json.JSONDecodeError):return {}

if __name__=='__main__':
    last=None
    while True:
        main=watch_final.snapshot()
        result=subprocess.run(['python3',str(ROOT/'audit_remaining_matrix.py')],capture_output=True,text=True)
        assert result.returncode==0, result.stderr
        extra=read(ROOT/'REMAINING_MATRIX_STATUS.json')
        baseline=read(ROOT/'knighter/MATRIX_FREEZE.json')
        paired=sum(read(ROOT/'knighter'/case/'PAIRED_RESULT.json').get('status')=='completed' for case in baseline['subjects'])
        live=subprocess.check_output(['docker','ps','--format','{{.Names}}','--filter','name=semweaver'],text=True).splitlines()
        status={'checked_at':datetime.datetime.now(datetime.timezone.utc).isoformat(),
                'main_complete_pairs':main['verified_pairs'],'main_planned':39,
                'e3_complete_new_cells':extra['e3']['verified_complete'],'e3_planned_new_cells':48,
                'e4_complete_cells':extra['e4']['verified_complete'],'e4_planned_cells':extra['e4']['planned_cells'],
                'baseline_complete_fresh_pairs':paired,'baseline_planned_fresh_pairs':len(baseline['subjects']),
                'live_containers':live,'boundary':'Completed means verified original or designated repair; no pending/failed outcome is imputed.'}
        tmp=ROOT/'STUDY_PROGRESS.tmp';tmp.write_text(json.dumps(status,indent=2));tmp.replace(ROOT/'STUDY_PROGRESS.json')
        signature=(main['verified_pairs'],extra['e3']['verified_complete'],extra['e4']['verified_complete'],paired,tuple(live))
        if signature!=last:
            with (ROOT/'STUDY_PROGRESS_EVENTS.jsonl').open('a') as handle:handle.write(json.dumps(status)+'\n')
            print(json.dumps(status),flush=True);last=signature
        if signature[:4]==(39,48,extra['e4']['planned_cells'],len(baseline['subjects'])):break
        time.sleep(60)
