"""Transparent raw-reply recorder around the unchanged profile algorithm."""
import importlib.util
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('wire_profile_runner', ROOT/'run_profile_cell.py')
profile = importlib.util.module_from_spec(spec)
spec.loader.exec_module(profile)
original_run = subprocess.run


def recorded_run(command, *args, **kwargs):
    env = kwargs.get('env')
    if env and env.get('SEMWEEVER_LLM_PROVIDER') == 'anthropic' and isinstance(command,(list,tuple)) and len(command)>1:
        script = Path(str(command[1])).name
        treatment = script == 'run_semweaver_treatment_case.py'
        arm = script == 'arm64_entry.py' and '--script' in command and command[command.index('--script')+1] == 'run_semweaver_treatment_case.py'
        if treatment or arm:
            arguments = list(command[2:]) if treatment else list(command[command.index('--')+1:])
            output = Path(arguments[arguments.index('--output-dir')+1])
            env = dict(env)
            env['SEMWEEVER_PROVIDER_WIRE_LOG'] = str(output/'provider_wire.jsonl')
            kwargs['env'] = env
            command = [command[0],str(ROOT/'provider_wire_entry.py'),'--source-root',str(profile.SOURCE),
                       '--architecture','arm64' if arm else 'x86','--',*arguments]
    return original_run(command,*args,**kwargs)


subprocess.run = recorded_run
if __name__ == '__main__':
    profile.cell(Path(sys.argv[1]))
