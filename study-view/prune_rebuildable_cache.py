"""Prune only inactive old-round object/plugin caches, preserving provenance."""
import hashlib
import json
import os
import sys
import time
from pathlib import Path

PROJECT=Path('/artifact/project')
ROOT=PROJECT/'final-native-20260928'
TARGETS=[PROJECT/name for name in ('native-state-20260928','native-pilot-20260928',
          'native-persistence-20260928','native-budget-response-20260928','native-trace-20260928')]
NAMES={'RUN_PLAN.json','MATRIX_FREEZE.json','RUN_MANIFEST.json','ADOPTION.json','SELECTION.json'}
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()


if __name__=='__main__':
    protected=set()
    def collect(value):
        if isinstance(value,dict):
            for k,v in value.items():
                if k in ('inputs','input_hashes','common_input_hashes') and isinstance(v,dict):
                    protected.update(str(Path(p).resolve()) for p in v if p.startswith('/'))
                collect(v)
        elif isinstance(value,list):
            for v in value:collect(v)
    for folder in [*TARGETS,ROOT]:
        for path in folder.rglob('*.json'):
            if path.name not in NAMES:continue
            try:collect(json.loads(path.read_text()))
            except (OSError,json.JSONDecodeError):continue
    candidates=[]
    for folder in TARGETS:
        assert folder.resolve()==folder and folder.parent==PROJECT
        for path in folder.rglob('*'):
            if path.is_symlink() or not path.is_file():continue
            if path.suffix!='.o' and path.name!='SAGenTestPlugin.so':continue
            assert path.resolve().is_relative_to(folder)
            if str(path.resolve()) in protected:continue
            if not os.access(path,os.W_OK) or not os.access(path.parent,os.W_OK):continue
            candidates.append({'path':str(path),'bytes':path.stat().st_size,'sha256':sha(path)})
    name=sys.argv[1] if len(sys.argv)>1 else 'REBUILDABLE_CACHE_PRUNE_20260929.json'
    assert name.startswith('REBUILDABLE_CACHE_PRUNE_20260929') and name.endswith('.json') and '/' not in name
    manifest=ROOT/name
    assert not manifest.exists()
    receipt={'created_at':time.time(),'exact_target_roots':[str(p) for p in TARGETS],
             'candidates':candidates,'protected_input_paths':len(protected),
             'boundary':'Only .o and SAGenTestPlugin.so from inactive historical rounds; no source, checker output, JSON, reply, report or log removed. Compiled files can be regenerated from retained source and image.'}
    with manifest.open('x') as handle:json.dump(receipt,handle,indent=2)
    deleted=[]
    for row in candidates:
        path=Path(row['path'])
        assert path.stat().st_size==row['bytes'] and sha(path)==row['sha256']
        path.unlink();deleted.append(row['path'])
    receipt.update(deleted_paths=deleted,deleted_bytes=sum(r['bytes'] for r in candidates),finished_at=time.time())
    temporary=manifest.with_suffix('.tmp')
    with temporary.open('x') as handle:json.dump(receipt,handle,indent=2)
    temporary.replace(manifest)
    print(json.dumps({'deleted_rebuildable_files':len(deleted),'regenerable_GiB':round(receipt['deleted_bytes']/2**30,2)}),flush=True)
