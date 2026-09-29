"""Apply the same verified relocation protocol to additional idle clones."""
from pathlib import Path

ROOT=Path(__file__).resolve().parent
original=ROOT/'move_idle_clones_to_hdd.py'
text=original.read_text()
old="names=[f'linux-screen-v8-promisor-{i}' for i in range(4)]+[f'linux-screen-v8-promisor2-{i}' for i in range(4)]"
new="names=[f'linux-screen-v8-shard-{i}' for i in range(4)]+[f'linux-screen-v8-mirror-{i}' for i in range(4)]"
assert text.count(old)==1
text=text.replace(old,new).replace("'HDD_COLD_RELOCATION.json'","'HDD_COLD_RELOCATION_SECOND.json'")
text=text.replace("'script_sha256':sha(Path(__file__)),","'script_sha256':sha(Path(__file__)), 'adapter_sha256':sha(ROOT/'move_more_idle_clones_to_hdd.py'),")
if __name__=='__main__':
    exec(compile(text,str(original)+'[additional-verified-idle-clones]','exec'),
         {'__name__':'__main__','__file__':str(original)})
