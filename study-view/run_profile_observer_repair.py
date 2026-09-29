"""Retain the profile algorithm and raw wire recorder with repaired probes."""
import importlib.util
import subprocess
import sys
from pathlib import Path

ROOT=Path(__file__).resolve().parent
original=subprocess.run
def run(command,*args,**kwargs):
    if isinstance(command,list) and len(command)>1 and Path(command[1]).name in ('run_semweaver_treatment_case.py','provider_wire_entry.py'):
        command=[command[0],str(ROOT/'repaired_pipeline_entry.py'),
                 '--source-root',str(ROOT.parent/'LLM-Native/SemWeaver-v43'),
                 '--target-script',command[1],'--',*command[2:]]
    return original(command,*args,**kwargs)
subprocess.run=run
spec=importlib.util.spec_from_file_location('profile_with_original_wire_recorder',ROOT/'run_profile_wire.py')
wire=importlib.util.module_from_spec(spec)
spec.loader.exec_module(wire)
if __name__=='__main__':wire.profile.cell(Path(sys.argv[1]))
