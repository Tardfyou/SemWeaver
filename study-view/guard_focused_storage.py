"""Storage guard for authorized focused queues, excluding retired full matrices."""
from pathlib import Path

ROOT=Path(__file__).resolve().parent
QUEUES={
    'resume_verified_queues.py','queue_contextless_repairs.py',
    'watch_observer_failures.py','launch_contextless_repair.py',
    'launch_observer_repair.py','launch_profile_observer_repair.py',
    'launch_crash_checkpoint_repair.py','launch_e3_parallel_control.py',
    'launch_e3_parallel_repeat3_native.py','launch_e3_parallel_repeat3_control.py',
    'launch_focused_robustness.py','launch_any_profile_cap_repair.py',
}

if __name__=='__main__':
    original=ROOT/'guard_storage.py';source=original.read_text()
    assert source.count('paused={}')==1
    source=source.replace('paused={}',f'QUEUE_NAMES={QUEUES!r}\npaused={{}}')
    source=source.replace('STORAGE_GUARD_POLICY.json','FOCUSED_STORAGE_GUARD_POLICY.json')
    source=source.replace('STORAGE_GUARD_EVENTS.jsonl','FOCUSED_STORAGE_GUARD_EVENTS.jsonl')
    exec(compile(source,str(original)+'[authorized-focused-queues]', 'exec'),
         {'__name__':'__main__','__file__':str(Path(__file__).resolve())})
