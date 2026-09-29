"""Increase disposable observer capacity without changing checker/scope/scorer."""
import argparse
import importlib.util
import json
import sys
from pathlib import Path
from fixed_trace_probe import repaired_instrumenter

ROOT=Path(__file__).resolve().parent


def expanded_header(original,cap):
    assert cap in (64000,256000)
    text=original.read_text()
    old='constexpr unsigned cap = 16000;'
    assert text.count(old)==1, 'Observer header drift'
    return text.replace(old,f'constexpr unsigned cap = {cap};')


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--source-root',type=Path,required=True)
    parser.add_argument('arguments',nargs=argparse.REMAINDER)
    args=parser.parse_args()
    arguments=args.arguments[1:] if args.arguments[:1]==['--'] else args.arguments
    output=Path(arguments[arguments.index('--output-dir')+1]);output.mkdir(parents=True,exist_ok=True)
    script=args.source_root/'experiments/knighter/experiment/run_checker_trace_probe.py'
    sys.path.insert(0,str(args.source_root))
    instrument,original_instrumenter_sha=repaired_instrumenter(args.source_root)
    rows=[];selected=None;rc=1
    headers=output/'headers';headers.mkdir(exist_ok=False)
    for cap in (64000,256000):
        target=output/f'cap-{cap}'
        # The original probe creates its output with exist_ok=False.
        header=headers/f'observer_support_{cap}.h'
        header.write_text(expanded_header(args.source_root/'src/research/checker_trace_support.h',cap))
        spec=importlib.util.spec_from_file_location(f'original_probe_cap_{cap}',script)
        probe=importlib.util.module_from_spec(spec);spec.loader.exec_module(probe)
        probe.instrument=lambda code,roots,support:instrument(code,roots,header)
        current=list(arguments);current[current.index('--output-dir')+1]=str(target)
        sys.argv=[str(script),*current]
        rc=probe.main()
        selected=target
        # Preserve the original receipts, then explicitly bind the actual adapter
        # and header used. No frozen input or scored output is overwritten.
        extra={str(header):probe.sha(header),str(Path(__file__)):probe.sha(Path(__file__)),
               str(ROOT/'fixed_trace_probe.py'):probe.sha(ROOT/'fixed_trace_probe.py')}
        for name in ('RUN_PLAN.json','RUN_MANIFEST.json'):
            path=target/name
            if not path.exists():continue
            raw=path.read_bytes();(target/('ORIGINAL_'+name)).write_bytes(raw)
            data=json.loads(raw);data['inputs']={**data.get('inputs',{}),**extra}
            data['observer_capacity_repair']={'actual_unique_event_cap':cap,
                 'original_instrumenter_sha256':original_instrumenter_sha,
                 'scored_checker_modified':False,'original_receipt_sha256':probe.sha(target/('ORIGINAL_'+name))}
            assert all(probe.sha(p)==h for p,h in data['inputs'].items())
            path.write_text(json.dumps(data,indent=2))
        result=json.loads((target/'RESULT.json').read_text()) if (target/'RESULT.json').exists() else {}
        rows.append({'cap':cap,'return_code':rc,'probe':str(target),
                     'execution_health':result.get('execution_health'),'diagnostic_parity':result.get('diagnostic_parity'),
                     'trace_usable':result.get('trace_usable'),'error':result.get('error')})
        if rc==0 and result.get('trace_usable'):break
        scans=result.get('scans',[])
        capped=(rc==0 and result.get('execution_health')=='completed' and result.get('diagnostic_parity')
                and len(scans)==4 and all(x['execution_valid'] for x in scans)
                and any(x.get('trace_capped') for x in scans))
        if not capped:break  # Other failures must be diagnosed, not hidden.
    for item in selected.iterdir():
        alias=output/item.name
        assert not alias.exists() and not alias.is_symlink()
        alias.symlink_to(item.relative_to(output))
    (output/'ADAPTIVE_OBSERVER.json').write_text(json.dumps({'attempts':rows,'selected_probe':str(selected),
        'model_calls':0,'same_function_scope':True,'scored_checker_modified':False,
        'boundary':'Complete uncapped observations required; no absence/safety inference, no truncated trace delivered as evidence.'},indent=2))
    result=json.loads((selected/'RESULT.json').read_text()) if (selected/'RESULT.json').exists() else {}
    return 0 if rc==0 and result.get('trace_usable') else 1


if __name__=='__main__':raise SystemExit(main())
