#!/usr/bin/env python3
"""Check bounded, owned confirmation fixture labels with AddressSanitizer."""
import argparse,csv,json,os,subprocess
from pathlib import Path

def main():
    p=argparse.ArgumentParser();p.add_argument('--suite',type=Path,required=True);p.add_argument('--output',type=Path,required=True);args=p.parse_args()
    if args.output.exists():raise SystemExit('Refusing overwrite')
    args.output.mkdir(parents=True)
    rows=list(csv.DictReader((args.suite/'manifest.csv').open()))
    results=[]
    for row in rows:
        name=row['variant_id'];exe=args.output/name
        build=subprocess.run(['/usr/lib/llvm-18/bin/clang','-O0','-g','-fsanitize=address','-fno-omit-frame-pointer',str(args.suite/'runtime'/f'{name}.c'),'-o',str(exe)],capture_output=True,text=True,timeout=30)
        (args.output/f'{name}.compile.log').write_text(build.stdout+build.stderr)
        if build.returncode:raise RuntimeError(f'Fixture compile failure: {name}')
        run=subprocess.run([str(exe)],env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'},capture_output=True,text=True,timeout=10)
        log=run.stdout+run.stderr;(args.output/f'{name}.runtime.log').write_text(log)
        observed='ERROR: AddressSanitizer: stack-buffer-overflow' in log
        expected=bool(int(row['expected_alert']))
        valid=observed or run.returncode==0
        results.append({'variant_id':name,'expected_oob':expected,'asan_stack_oob':observed,'return_code':run.returncode,'execution_valid':valid,'passed':valid and observed==expected})
    result={'method':'bounded-owned-fixture-asan-label-check','cases':len(results),'passed':sum(r['passed'] for r in results),'all_passed':all(r['passed'] for r in results),'results':results}
    (args.output/'RESULT.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({k:v for k,v in result.items() if k!='results'}))

if __name__=='__main__':main()
