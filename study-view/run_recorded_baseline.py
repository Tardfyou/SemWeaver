"""Read the authorized private key inside the container, never serialize it."""
import os
import runpy
import sys
from pathlib import Path

key = Path('/run/private/gpt-key')
assert key.stat().st_mode & 0o077 == 0
os.environ.update(CUSTOM_OPENAI_API_KEY=key.read_text().strip(),
                  CUSTOM_OPENAI_BASE_URL='https://model-gateway.example.invalid/v1',
                  CUSTOM_OPENAI_TIMEOUT='1800')
script = Path(__file__).resolve().parent.parent / 'native-state-20260928/record_knighter_exchanges.py'
sys.argv[0] = str(script)
runpy.run_path(str(script), run_name='__main__')
