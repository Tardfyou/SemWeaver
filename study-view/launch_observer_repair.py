"""Resume verified prefixes using the corrected, isolated observer."""
from pathlib import Path
import launch_contextless_repair as launcher

ROOT = Path(__file__).resolve().parent
original_save = launcher.save
original_run = launcher.subprocess.run
extra = {str(ROOT/name): launcher.sha(ROOT/name) for name in
         ('launch_observer_repair.py', 'fixed_trace_probe.py',
          'repaired_treatment_entry.py', 'run_observer_repaired_cell.py')}
def save(path, data):
    if path.name in ('RUN_PLAN.json','RUN_MANIFEST.json'):
        data = {**data, 'inputs': {**data['inputs'], **extra},
                'operational_repair': 'Observer-only parameter-shadowing repair; same model, cadence, budget, scorer and adoption rules. Original failure preserved.'}
    original_save(path, data)
def run(command, *args, **kwargs):
    if isinstance(command,list) and str(ROOT/'run_contextless_repair.py') in command:
        command = list(command)
        command[command.index(str(ROOT/'run_contextless_repair.py'))] = str(ROOT/'run_observer_repaired_cell.py')
    return original_run(command,*args,**kwargs)
launcher.save = save
launcher.subprocess.run = run
if __name__=='__main__':
    launcher.main()
