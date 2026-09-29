"""Tiny compatibility check for a common E4 output ceiling, not efficacy."""
import json
import os
from pathlib import Path
from langchain_core.messages import HumanMessage
from src.utils.config import load_config
from src.llm.langchain_builder import build_langchain_chat_model

ROOT = Path(__file__).resolve().parent
PROJECT = ROOT.parent
key = Path('/run/private/model-key')
assert key.stat().st_mode & 0o077 == 0
os.environ.update(SEMWEEVER_MODEL='gpt-6-luna', SEMWEEVER_REVIEW_MODEL='gpt-6-luna',
                  SEMWEEVER_LLM_PROVIDER='openai_compatible', SEMWEEVER_WIRE_API='responses',
                  SEMWEEVER_REASONING_EFFORT='high', OPENAI_API_KEY=key.read_text().strip(),
                  OPENAI_BASE_URL='https://model-gateway.example.invalid/v1')
config = load_config(str(PROJECT / 'LLM-Native/SemWeaver-v43/config/config.yaml'))
config['llm']['generation'].update(timeout=180, max_retries=0)
rows = []
for ceiling in (65536, 32768):
    config['llm']['refine']['max_tokens'] = ceiling
    model = build_langchain_chat_model(config=config, generation_config_key='refine', temperature_override=0)
    try:
        result = model.invoke([HumanMessage(content='Return only JSON: {"action":"finish","summary":"capacity smoke"}')], response_format={'type':'json_object'})
        assert json.loads(result.content)['action'] == 'finish'
        rows.append({'ceiling':ceiling,'status':'pass','usage':result.usage_metadata})
        break
    except Exception as error:
        rows.append({'ceiling':ceiling,'status':'error','error_type':type(error).__name__,
                     'status_code':getattr(error,'status_code',None)})
payload = {'model':'gpt-6-luna','reasoning_effort':'high','rows':rows,
           'scope':'Compatibility only; no checker result generated.'}
with (ROOT/'LUNA_OUTPUT_CAPACITY_SMOKE.json').open('x') as handle:
    json.dump(payload,handle,indent=2)
    handle.write('\n')
print(json.dumps(payload))
