"""Run the original treatment/wire recorder with an uncapped observer bridge."""
import argparse
import runpy
import subprocess
import sys
from pathlib import Path

if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--source-root',type=Path,required=True)
    parser.add_argument('--target-script',type=Path,required=True)
    parser.add_argument('arguments',nargs=argparse.REMAINDER)
    args=parser.parse_args();original=subprocess.Popen
    def popen(command,*positional,**kwargs):
        if isinstance(command,list) and len(command)>1 and Path(command[1]).name=='run_checker_trace_probe.py':
            command=[command[0],str(Path(__file__).with_name('adaptive_trace_probe.py')),
                     '--source-root',str(args.source_root),'--',*command[2:]]
        return original(command,*positional,**kwargs)
    subprocess.Popen=popen
    arguments=args.arguments[1:] if args.arguments[:1]==['--'] else args.arguments
    sys.argv=[str(args.target_script),*arguments]
    sys.path.insert(0,str(args.source_root))
    runpy.run_path(str(args.target_script),run_name='__main__')
