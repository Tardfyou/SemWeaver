"""Close evidence/cost ledgers only after all39 main pairs are verified."""
import json
import subprocess
import time
from pathlib import Path
import watch_final

ROOT=Path(__file__).resolve().parent

if __name__=='__main__':
    while True:
        state=watch_final.snapshot()
        if state['verified_pairs']==39:break
        time.sleep(60)
    for script in ('collect_native_availability.py','collect_main_costs.py'):
        subprocess.run(['python3',str(ROOT/script)],check=True)
    availability=json.loads((ROOT/'NATIVE_EVIDENCE_AVAILABILITY_STATUS.json').read_text())
    costs=json.loads((ROOT/'MAIN_COST_STATUS.json').read_text())
    assert availability['status']=='complete' and availability['completed_pairs']==39
    assert availability['integrity_error_attempts']==0, 'Inspect evidence errors; never fill or bypass'
    assert costs['status']=='complete' and costs['completed_pairs']==39
    print(json.dumps({'main_pairs':39,'evidence_ledger':'verified',
                      'cost_boundary':'Selected scored chains, not full operational billing'}),flush=True)
