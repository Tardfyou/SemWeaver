"""Relocate verified-idle screening clones; preserve every file and old paths."""
import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile
import time
from pathlib import Path

PROJECT=Path('/artifact/project')
ROOT=PROJECT/'final-native-20260928'
EXTERNAL=PROJECT/'LLM-Native/SemWeaver/artifacts/external'

def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def verified_copy(source,destination):
    result=shutil.copy2(source,destination)
    assert sha(source)==sha(destination), 'Copy verification failed; original retained'
    return result


if __name__=='__main__':
    assert Path('/mnt/hdd').is_mount()
    owner=PROJECT.stat()
    cold=Path(tempfile.mkdtemp(prefix='semweaver-cold-20260929.',dir='/mnt/hdd'))
    os.chown(cold,owner.st_uid,owner.st_gid)
    names=[f'linux-screen-v8-promisor-{i}' for i in range(4)]+[f'linux-screen-v8-promisor2-{i}' for i in range(4)]
    # These are not experimental source lanes or frozen evidence inputs.
    inputs=set()
    for freeze in [ROOT/'MATRIX_FREEZE.json',ROOT/'e3-selected-repeats/MATRIX_FREEZE.json',
                   ROOT/'e4-common64/MATRIX_FREEZE.json',ROOT/'knighter/MATRIX_FREEZE.json']:
        d=json.loads(freeze.read_text())
        inputs.update(d.get('common_input_hashes',{}));inputs.update(d.get('input_hashes',{}))
    assert not any(name in p for name in names for p in inputs)
    active_text=[]
    for folder in [PROJECT/'LLM-Native/artifacts/fse_revision/generate_only_evidence_all39_v8_r5',
                   PROJECT/'native-trace-20260928/adoption']:
        for p in folder.rglob('*'):
            if p.is_file() and p.suffix in ('.json','.jsonl','.txt','.md','.yaml'):
                try:active_text.append(p.read_text())
                except UnicodeError:pass
    assert not any(name in text for name in names for text in active_text)
    for cfg in ROOT.rglob('CONFIG.json'):
        if 'continuation-' in str(cfg):continue
        d=json.loads(cfg.read_text())
        assert not any(name in d.get('linux_dir','') for name in names)
    plan={'created_at':time.time(),'cold_root':str(cold),'names':names,'moves':[],
          'script_sha256':sha(Path(__file__)),
          'boundary':'Idle screening clones only; no running kernel lane, evidence, model output, source checker or dataset removed. Each copied file hash checked before deleting old copy. Original path becomes a symlink; relocation is recoverable.'}
    ledger=ROOT/'HDD_COLD_RELOCATION.json'
    assert not ledger.exists()
    with ledger.open('x') as handle:json.dump(plan,handle,indent=2)
    for name in names:
        source=EXTERNAL/name
        assert source.parent==EXTERNAL and source.is_dir() and not source.is_symlink() and source.resolve()==source
        destination=cold/name
        assert not destination.exists()
        head=subprocess.check_output(['git','-c',f'safe.directory={source}','-C',str(source),'rev-parse','HEAD'],text=True).strip()
        shutil.copytree(source,destination,symlinks=True,copy_function=verified_copy)
        before={str(p.relative_to(source)):(p.lstat().st_size,os.readlink(p) if p.is_symlink() else None)
                for p in source.rglob('*') if p.is_file() or p.is_symlink()}
        after={str(p.relative_to(destination)):(p.lstat().st_size,os.readlink(p) if p.is_symlink() else None)
               for p in destination.rglob('*') if p.is_file() or p.is_symlink()}
        assert before==after
        # Exact source directory only; the verified complete destination exists.
        shutil.rmtree(source)
        source.symlink_to(destination,target_is_directory=True)
        observed=subprocess.check_output(['git','-c',f'safe.directory={source}','-C',str(source),'rev-parse','HEAD'],text=True).strip()
        assert observed==head
        row={'original':str(source),'destination':str(destination),'git_head':head,
             'files':len(before),'logical_bytes':sum(s[0] for s in before.values()),'finished_at':time.time()}
        plan['moves'].append(row)
        temporary=ledger.with_suffix('.tmp')
        with temporary.open('w') as handle:json.dump(plan,handle,indent=2)
        temporary.replace(ledger)
        print(json.dumps({'clone_relocated':name,'cold_root':str(cold),'logical_GiB':round(row['logical_bytes']/2**30,2)}),flush=True)
