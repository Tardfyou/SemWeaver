"""Verify output-budget compatibility on the actual Responses transport."""
import json
from pathlib import Path
from openai import OpenAI

ROOT = Path(__file__).resolve().parent
key = Path('/run/private/model-key')
assert key.stat().st_mode & 0o077 == 0
client = OpenAI(api_key=key.read_text().strip(),base_url='https://model-gateway.example.invalid/v1',timeout=180,max_retries=0)
rows = []
for ceiling in (65536,32768):
    try:
        response = client.responses.create(model='gpt-6-luna',input='Return only JSON: {"action":"finish","summary":"capacity smoke"}',
            reasoning={'effort':'high'},max_output_tokens=ceiling,text={'format':{'type':'json_object'}})
        assert json.loads(response.output_text)['action']=='finish'
        rows.append({'ceiling':ceiling,'status':'pass','usage':response.usage.model_dump() if response.usage else None})
        break
    except Exception as error:
        rows.append({'ceiling':ceiling,'status':'error','error_type':type(error).__name__,
                     'status_code':getattr(error,'status_code',None)})
payload={'scope':'Actual Responses compatibility only, not checker efficacy','rows':rows,'sdk_retries':0}
with (ROOT/'LUNA_DIRECT_CAPACITY_SMOKE.json').open('x') as handle:
    json.dump(payload,handle,indent=2)
    handle.write('\n')
print(json.dumps(payload))
