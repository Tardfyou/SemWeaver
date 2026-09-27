#!/usr/bin/env python3
"""Freeze post-hoc G21 fixtures before replaying two unmodified checkers.

Core labels follow a constant-array invariant: for i=0..B-1, i+1 reaches B;
an unguarded look-ahead is in bounds exactly when B<N (for positive B,N).
Boundary cases deliberately probe inherited guard and alias limitations.
This is an illustrative development diagnostic, not held-out evaluation.
"""
import csv
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent
OUT = ROOT / 'LLM-Native/artifacts/fse_revision/motivating_g21_selection_20260927'
DATA = ROOT / 'LLM-Native/artifacts/fse_revision'
CHECKERS = {
    'baseline': DATA / 'generate_only_materialized_v1/cases/G21_97cba232549b_Buffer_Overflow/csa/SAGenTestChecker.cpp',
    'refined_flash': DATA / 'e4_all39_glm_5_3_flash_precision_v10_r1/G21_97cba232549b_Buffer_Overflow/attempt-01/SAGenTestChecker.cpp',
}

def sha(data):
    return hashlib.sha256(data).hexdigest()

def program(cap=8, fixed=False, rename=False, inserted=False, nested=False,
            index='i + 1', reverse=False, inc='++i', bound=None, local=False):
    typ, fn, arr, iv = ('entry', 'inspect', 'items', 'cursor') if rename else ('link', 'check_links', 'links', 'i')
    upper = bound or ('CAP - 1' if fixed else 'CAP')
    idx = index.replace('i', iv)
    condition = f'({upper}) > {iv}' if reverse else f'{iv} < ({upper})'
    increment = inc.replace('i', iv)
    base = arr if local else f'dc->{arr}'
    prefix = f'enum {{ CAP = {cap} }};\nstruct {typ} {{ int value; }};\nextern void consume(struct {typ} *);\nextern void observe(unsigned);\n'
    if local:
        prefix += f'void {fn}(struct {typ} *seed) {{\n  struct {typ} *{arr}[CAP] = {{0}};\n  {arr}[0] = seed;\n'
    else:
        prefix += f'struct context {{ struct {typ} *{arr}[CAP]; }};\nvoid {fn}(struct context *dc) {{\n'
    prefix += f'  for (unsigned {iv} = 0; {condition}; {increment}) {{\n'
    if inserted:
        prefix += f'    observe({iv});\n'
    if nested:
        prefix += '    {\n'
    prefix += f'    consume({base}[{idx}]);\n'
    if nested:
        prefix += '    }\n'
    return prefix + '  }\n}\n'

def main():
    if OUT.exists():
        raise SystemExit(f'Refusing overwrite: {OUT}')
    OUT.mkdir(parents=True)
    rows=[]
    configurations = [
        ('canonical', {}), ('renamed_symbols', {'rename':True}),
        ('independent_statement', {'inserted':True}), ('nested_block', {'nested':True}),
        ('commuted_index', {'index':'1 + i'}), ('reversed_loop_test', {'reverse':True}),
        ('post_increment', {'inc':'i++'}), ('compound_increment', {'inc':'i += 1'}),
        ('parenthesized_index', {'index':'((i) + (1))'}),
        ('capacity_3', {'cap':3}), ('capacity_17', {'cap':17}),
        ('local_array', {'local':True}),
        ('all_combined', {'rename':True,'inserted':True,'nested':True,'reverse':True,'inc':'i++','index':'1 + i','cap':17}),
    ]
    def add(group, name, expected, source, meaning):
        path=OUT/group/'fixtures'/f'{name}.c'
        path.parent.mkdir(parents=True,exist_ok=True)
        path.write_text(source)
        rows.append({'group':group,'variant_id':name,'variant_kind':'positive' if expected else 'negative','transformation':meaning,'source_path':f'fixtures/{name}.c','expected_alert':int(expected),'source_sha256':sha(source.encode())})
    for name,kwargs in configurations:
        for fixed in (False,True):
            add('core',name+('_fixed' if fixed else '_vulnerable'),not fixed,program(fixed=fixed,**kwargs),name)
    # Both bounds contain subtraction; only actual array capacity disambiguates.
    add('core','minus_one_still_unsafe',True,program(bound='(CAP + 1) - 1'),'subtracted-one expression still permits index CAP')
    add('core','equivalent_safe_bound',False,program(bound='(CAP + 1) - 2'),'safe bound without an X-1 syntactic match')
    shared='enum { CAP=8 }; extern void use(int);\n'
    boundary={
      'pointer_alias_unsafe':(True, 'void f(void) { int storage[CAP]; int *p=storage; for(int i=0;i<(CAP+1)-1;++i) use(p[i+1]); }', 'unknown backing capacity through a pointer alias'),
      'unsafe_guard_direction':(True, 'void f(void) { int a[CAP]; for(int i=0;i<CAP;++i) if(i+1>=CAP) use(a[i+1]); }', 'relational guard points toward the unsafe index'),
      'unsafe_disjunction':(True, 'void f(int flag) { int a[CAP]; for(int i=0;i<CAP;++i) if(i+1<CAP || flag) use(a[i+1]); }', 'a disjunction does not guarantee bounds'),
      'safe_body_guard':(False, 'void f(void) { int a[CAP]; for(int i=0;i<CAP;++i) if(i+1<CAP) use(a[i+1]); }', 'body-level safe guard'),
      'safe_continue_guard':(False, 'void f(void) { int a[CAP]; for(int i=0;i<CAP;++i) { if(i+1>=CAP) continue; use(a[i+1]); } }', 'equivalent safe guard with continue'),
      'unsafe_non_strict_loop':(True, 'void f(void) { int a[CAP]; for(int i=0;i<=CAP-1;++i) use(a[i+1]); }', 'equivalent unsafe non-strict loop condition'),
    }
    for name,(expected,body,meaning) in boundary.items():
        add('boundary',name,expected,shared+body+'\n',meaning)
    for group in ('core','boundary'):
        selected=[r for r in rows if r['group']==group]
        with (OUT/group/'manifest.csv').open('w',newline='') as f:
            w=csv.DictWriter(f,fieldnames=['variant_id','variant_kind','transformation','source_path','expected_alert'])
            w.writeheader()
            w.writerows({k:r[k] for k in w.fieldnames} for r in selected)
    manifest={'schema_version':1,'purpose':'post-hoc motivating-example selection; no checker edits','case_id':'G21_97cba232549b_Buffer_Overflow','selection_rule':'inspect actual capacity-based automatic repair, retain all core and boundary outcomes','core_label_oracle':'for positive constant B and N, canonical i<B and a[i+1] has maximum index B, so unsafe iff B>=N','checkers':{k:{'path':str(p),'sha256':sha(p.read_bytes())} for k,p in CHECKERS.items()},'fixtures':rows}
    (OUT/'SELECTION_MANIFEST.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(json.dumps({'output':str(OUT),'core':sum(r['group']=='core' for r in rows),'boundary':sum(r['group']=='boundary' for r in rows)}))

if __name__=='__main__':
    main()
