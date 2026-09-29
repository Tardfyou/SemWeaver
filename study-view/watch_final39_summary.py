"""Build the complete main/baseline table once prerequisites genuinely close."""
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
    while not (ROOT/'FINAL39_SUMMARY.json').exists():
        snapshot=watch_final.snapshot();subjects=read(ROOT/'knighter/MATRIX_FREEZE.json')['subjects']
        ready=(snapshot['verified_pairs']==39 and all(read(ROOT/'knighter'/c/'PAIRED_RESULT.json').get('status')=='completed' for c in subjects)
               and read(ROOT/'knighter-no-report/RESULT.json').get('status')=='completed')
        if ready:
            command=['python3',str(ROOT/'build_final39_summary.py')]
            with (ROOT/'final39-builder.stdout').open('x') as stdout,(ROOT/'final39-builder.stderr').open('x') as stderr:
                result=subprocess.run(command,stdout=stdout,stderr=stderr)
            print(json.dumps({'builder_return_code':result.returncode,'full_table_written':(ROOT/'FINAL39_SUMMARY.json').exists()}),flush=True)
            break  # A verifier failure needs diagnosis, not repeated overwrites.
        time.sleep(60)
