"""Disable transport dispatch reservation only for a network-disabled replay.

The received-response budget still belongs to the frozen outer profile; the
original entry forbids client construction and consumes the saved reply once.
"""
import os
from pathlib import Path
import runpy


if __name__ == '__main__':
    os.environ.pop('SEMWEEVER_PILOT_BUDGET_FILE', None)
    original = Path(__file__).with_name('replay_received_summary_entry.py')
    runpy.run_path(str(original), run_name='__main__')
