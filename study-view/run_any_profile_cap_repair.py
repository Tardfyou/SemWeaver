"""Use the capacity bridge for either a fresh zero-response cell or a prefix."""
import sys
from pathlib import Path
import run_profile_cap_repair as base

if __name__=='__main__':
    path=Path(sys.argv[1]);cfg=base.profile.read(path)
    if cfg.get('prefix_calls',0):
        namespace={**base.profile.__dict__,'__name__':'audited_profile_continuation','restore_prefix':base.restore_prefix}
        exec(compile(base.patched_source(),str(base.ROOT/'run_profile_cell.py')+'[audited-prefix]','exec'),namespace)
        namespace['cell'](path)
    else:
        assert not cfg.get('prefix_attempts')
        base.profile.cell(path)
