"""Fix feedback receipt fields before the remaining model call; preserve v1.

The v1 continuation was rejected pre-dispatch (zero new replies/requests).
Restore exact method/objective/case from the actual failed treatment receipt.
"""
from pathlib import Path

ROOT=Path(__file__).resolve().parent

if __name__=='__main__':
    original=ROOT/'launch_received_format_prefix.py';source=original.read_text()
    changes={
        "output=ROOT/'profile-repairs'/relative":"output=ROOT/'profile-repairs'/relative/'feedback-schema-repair'",
        "manifest={'model':cfg['profile']['model']":
        "manifest={**{k:failed_record[k] for k in ('method','objective','evidence_mode','case_id')},'model':cfg['profile']['model']",
        "paths=[Path(__file__),ROOT/'run_mapped_high_received_prefix.py'":
        "paths=[Path(__file__),ROOT/'launch_received_format_prefix.py',ROOT/'profile-repairs'/relative/'RUN_MANIFEST.json',ROOT/'run_mapped_high_received_prefix.py'",
    }
    for old,new in changes.items():
        assert source.count(old)==1,'Original prefix launcher drift'
        source=source.replace(old,new)
    exec(compile(source,str(original)+'[feedback-method-fields]', 'exec'),
         {'__name__':'__main__','__file__':str(Path(__file__).resolve())})
