#!/usr/bin/env python3
"""Freeze additional, unseen-by-editor bounded C probes and runtime oracles."""
import csv
import hashlib
import json
from pathlib import Path

ROOT=Path(__file__).resolve().parent/'LLM-Native/artifacts/fse_revision/motivating_g21_selection_20260927/confirmation'
CASES=[
 ('canonical_unsafe',1,'i<CAP','use(a[i+1]);'),
 ('canonical_safe',0,'i<CAP-1','use(a[i+1]);'),
 ('le_unsafe',1,'i<=CAP-1','use(a[i+1]);'),
 ('le_safe',0,'i<=CAP-2','use(a[i+1]);'),
 ('reversed_le_unsafe',1,'CAP-1>=i','use(a[i+1]);'),
 ('reversed_le_safe',0,'CAP-2>=i','use(a[i+1]);'),
 ('continue_safe',0,'i<CAP','if(i+1>=CAP) continue; use(a[i+1]);'),
 ('continue_inverted_unsafe',1,'i<CAP','if(i+1<CAP) continue; use(a[i+1]);'),
 ('break_safe',0,'i<CAP','if(i>=CAP-1) break; use(a[i+1]);'),
 ('conditional_break_unsafe',1,'i<CAP','if(flag) { if(i>=CAP-1) break; } use(a[i+1]);'),
 ('unsafe_guard_direction',1,'i<CAP','if(i+1>=CAP) use(a[i+1]);'),
 ('safe_then',0,'i<CAP','if(i+1<CAP) use(a[i+1]);'),
 ('safe_else',0,'i<CAP','if(i+1>=CAP) { use(0); } else { use(a[i+1]); }'),
 ('unsafe_else',1,'i<CAP','if(i+1<CAP) { use(0); } else { use(a[i+1]); }'),
 ('unsafe_or',1,'i<CAP','if(i+1<CAP || flag) use(a[i+1]);'),
 ('safe_both_or',0,'i<CAP','if(i+1<CAP || i<CAP-1) use(a[i+1]);'),
 ('safe_and',0,'i<CAP','if(flag && i+1<CAP) use(a[i+1]);'),
 ('wrong_larger_bound',1,'i<CAP','if(i+1<CAP+2) use(a[i+1]);'),
 ('conditional_continue_unsafe',1,'i<CAP','if(flag) { if(i+1>=CAP) continue; } use(a[i+1]);'),
 ('nested_dominating_continue_safe',0,'i<CAP','{ if(i+1>=CAP) continue; use(a[i+1]); }'),
]

def main():
    if ROOT.exists(): raise SystemExit(f'Refusing overwrite: {ROOT}')
    (ROOT/'fixtures').mkdir(parents=True)
    (ROOT/'runtime').mkdir()
    rows=[]
    for name,expected,condition,body in CASES:
        source='enum { CAP=11 };\nextern void use(int);\nvoid f(int flag) {\n  int a[CAP]={0};\n  for(int i=0; '+condition+'; ++i) { '+body+' }\n}\n'
        path=ROOT/'fixtures'/f'{name}.c';path.write_text(source)
        runtime=source+'\nvolatile int sink;\n__attribute__((noinline)) void use(int x) { sink ^= x; }\nint main(void) { f(0); f(1); return 0; }\n'
        (ROOT/'runtime'/f'{name}.c').write_text(runtime)
        rows.append({'variant_id':name,'variant_kind':'positive' if expected else 'negative','transformation':name,'source_path':f'fixtures/{name}.c','expected_alert':expected})
    with (ROOT/'manifest.csv').open('w',newline='') as f:
        w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
    (ROOT/'FROZEN_DESIGN.json').write_text(json.dumps({'purpose':'additional development confirmation, fixtures not supplied to model','label_rule':'bounded C loops with CAP=11; runtime AddressSanitizer checks accompany expected bounds labels','fixtures':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(ROOT.rglob('*.c'))}},indent=2)+'\n')
    print(json.dumps({'fixtures':len(rows),'directory':str(ROOT)}))

if __name__=='__main__':main()
