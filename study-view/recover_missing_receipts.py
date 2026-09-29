"""Verify completed artifacts when a terminated parent lost its outer receipt."""
import importlib.util
from pathlib import Path
import recover_outer_manifest as audit

ROOT=Path(__file__).resolve().parent
original_main=audit.main

def recover(cell):
    receipt=cell/'RUN_MANIFEST.json'
    assert not receipt.exists()
    # Run the existing strict audit without making a placeholder receipt.
    source=Path(audit.__file__).read_text()
    source=source.replace("assert receipt.exists() and receipt.stat().st_size == 0", "assert not receipt.exists()")
    source=source.replace("with receipt.open('w') as handle:", "with receipt.open('x') as handle:")
    source=source.replace("Launcher receipt was empty after disk exhaustion.", "Parent launcher exited before persisting its receipt.")
    source=source.replace("metadata = {**plan,", "metadata = {**plan, 'receipt_audit_wrapper_sha256': sha(Path(__file__).with_name('recover_missing_receipts.py')),")
    namespace={'__name__':'receipt_recovery_audit','__file__':audit.__file__}
    exec(compile(source,audit.__file__+'[missing-receipt-audit]','exec'),namespace)
    import sys
    saved=sys.argv
    try:
        sys.argv=[audit.__file__,str(cell)]
        namespace['main']()
    finally:
        sys.argv=saved

if __name__=='__main__':
    for group in ('runs','e3-selected-repeats','e4-common64'):
        for result in (ROOT/group).rglob('RESULT.json'):
            cell=result.parent
            if (cell/'RUN_PLAN.json').exists() and (cell/'CONFIG.json').exists() and not (cell/'RUN_MANIFEST.json').exists():
                recover(cell)
