"""Wait on verified matrix completion; do not restart workers or publish."""
import json
import subprocess
import time
from pathlib import Path

ROOT=Path(__file__).resolve().parent

if __name__=='__main__':
    while True:
        data=json.loads((ROOT/'STUDY_PROGRESS.json').read_text())
        if data['main_complete_pairs']==39 and data['e3_complete_new_cells']==48 and data['e4_complete_cells']==36:
            result=subprocess.run(['python3',str(ROOT/'build_auxiliary_summary.py')],capture_output=True,text=True)
            print(json.dumps({'return_code':result.returncode,'stdout':result.stdout,'stderr':result.stderr}),flush=True)
            break  # A failed integrity gate requires inspection, not an overwrite.
        time.sleep(60)
