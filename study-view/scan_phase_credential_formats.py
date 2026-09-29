"""Read-only indicative secret-format scan; print paths, never matched values.

Not a final anonymity guarantee. Container-owned crash logs require a root
reader; run with only the selected phase mounted read-only and no network/key.
"""
import json
import re
from pathlib import Path

ROOT=Path(__file__).resolve().parent
PATTERN=re.compile(rb'(?:sk-[A-Za-z0-9]{20,}|olp_[A-Za-z0-9]{20,}|\b[a-f0-9]{32}\.[A-Za-z0-9]{16,}\b)')
SUFFIXES={'.json','.jsonl','.md','.py','.log','.stderr','.stdout','.cpp','.h','.csv','.txt'}
SKIP={'build','cmake-build','CMakeFiles','__pycache__','trace_build','trace_backend','original_backend','validation_backend'}

if __name__=='__main__':
    count=0;possible=[];unreadable=[]
    for path in ROOT.rglob('*'):
        if not path.is_file() or path.is_symlink() or path.suffix not in SUFFIXES:continue
        relative=path.relative_to(ROOT)
        if any(part in SKIP for part in relative.parts):continue
        count+=1;carry=b'';found=False
        try:
            with path.open('rb') as handle:
                while True:
                    chunk=handle.read(1024*1024)
                    if not chunk:break
                    data=carry+chunk
                    if PATTERN.search(data):found=True;break
                    carry=data[-200:]
        except PermissionError:
            unreadable.append(str(relative));continue
        if found:possible.append(str(relative))
    print(json.dumps({'status':'complete_indicative_scan' if not unreadable else 'incomplete_permissions',
        'phase_text_files_seen':count,'possible_credential_file_paths':possible,'unreadable_files':unreadable,
        'boundary':'Selected current-phase text and known credential formats only; no matched values printed, no files modified. Live files can change; final exported source/evidence/PDF anonymity verification still required.'}))
