"""Two tiny structured-output compatibility checks, not effect experiments."""
import hashlib
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
os.environ.update(SEMWEEVER_LLM_PROVIDER='anthropic', SEMWEEVER_WIRE_API='anthropic_messages',
                  SEMWEEVER_REASONING_EFFORT='', ANTHROPIC_AUTH_TOKEN=key.read_text().strip(),
                  ANTHROPIC_BASE_URL='https://open.bigmodel.cn/api/anthropic')
output = ROOT / 'GLM_PROFILE_SMOKE.json'
assert not output.exists()
rows = []
for name in ('glm-5.3', 'glm-5.3-flash'):
    os.environ.update(SEMWEEVER_MODEL=name, SEMWEEVER_REVIEW_MODEL=name)
    config = load_config(str(PROJECT / 'LLM-Native/SemWeaver-v43/config/config.yaml'))
    config['llm']['generation'].update(timeout=180, max_retries=0)
    config['llm']['refine']['max_tokens'] = 16384
    model = build_langchain_chat_model(config=config, generation_config_key='refine', temperature_override=0)
    assert model.model_name == name and model.max_tokens == 16384
    try:
        answer = model.invoke([HumanMessage(content='Return one JSON object with action="finish" and summary="compatibility smoke". Do not include any other fields.')], response_format={'type':'json_object'})
        raw = str(answer.content)
        payload = json.loads(raw)
        assert payload['action'] == 'finish' and isinstance(payload['summary'], str)
        rows.append({'model': name, 'status': 'pass', 'response_sha256': hashlib.sha256(raw.encode()).hexdigest(),
                     'usage_metadata': answer.usage_metadata})
    except Exception as error:
        rows.append({'model': name, 'status': 'error', 'error_type': type(error).__name__,
                     'status_code': getattr(error,'status_code',None)})
result = {'scope':'Provider/structured-output smoke only, not checker effectiveness', 'rows':rows,
          'output_token_ceiling':16384,'attempts':2,'sdk_retries':0}
with output.open('x') as handle:
    json.dump(result,handle,indent=2)
    handle.write('\n')
print(json.dumps(result))
