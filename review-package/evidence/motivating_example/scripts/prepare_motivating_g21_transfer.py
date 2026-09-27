#!/usr/bin/env python3
"""Freeze renamed/capacity-varied confirmation executions; no checker edits."""
import csv,hashlib,json,re
from pathlib import Path

BASE=Path(__file__).resolve().parent/'LLM-Native/artifacts/fse_revision/motivating_g21_selection_20260927'
OUT=BASE/'transfer_confirmation'

def main():
    if OUT.exists():raise SystemExit(f'Refusing overwrite: {OUT}')
    (OUT/'fixtures').mkdir(parents=True);(OUT/'runtime').mkdir()
    seeds=list(csv.DictReader((BASE/'confirmation/manifest.csv').open()))
    rows=[]
    for capacity in (3,7,19):
        for seed in seeds:
            source=(BASE/'confirmation'/seed['source_path']).read_text().replace('CAP=11',f'CAP={capacity}')
            names={'CAP':'EXTENT','f':'inspect_window','a':'slots','i':'cursor','flag':'enabled','use':'observe_value'}
            source=re.sub(r'\b(?:CAP|f|a|i|flag|use)\b',lambda m:names[m.group()],source)
            # A side-effect-free scalar observation is unrelated to bounds.
            source=source.replace('++cursor) {','++cursor) { observe_value(enabled);')
            name=f'cap{capacity}_{seed["variant_id"]}'
            (OUT/'fixtures'/f'{name}.c').write_text(source)
            runtime=source+'\nvolatile int sink;\n__attribute__((noinline)) void observe_value(int x) { sink ^= x; }\nint main(void) { inspect_window(0); inspect_window(1); return 0; }\n'
            (OUT/'runtime'/f'{name}.c').write_text(runtime)
            rows.append({**seed,'variant_id':name,'transformation':f'rename all local names, capacity {capacity}, insert unrelated scalar observation','source_path':f'fixtures/{name}.c'})
    with (OUT/'manifest.csv').open('w',newline='') as f:
        w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
    (OUT/'FROZEN_DESIGN.json').write_text(json.dumps({'purpose':'finite development confirmation after additional automatic repair; 20 shapes at three capacities, not 60 independent bugs','fixture_count':len(rows),'source_hashes':{str(p.relative_to(OUT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(OUT.rglob('*.c'))}},indent=2)+'\n')
    print(json.dumps({'fixtures':len(rows),'directory':str(OUT)}))

if __name__=='__main__':main()
