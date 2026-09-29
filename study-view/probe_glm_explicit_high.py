"""Small interface smoke with explicit high effort; not checker efficacy data."""
import json
import time
from pathlib import Path
from anthropic import Anthropic

ROOT=Path(__file__).resolve().parent
key=Path('/run/private/model-key');assert key.stat().st_mode&0o077==0
client=Anthropic(auth_token=key.read_text().strip(),base_url='https://open.bigmodel.cn/api/anthropic',timeout=300,max_retries=0)
rows=[]
for model in ('glm-5.3-flash','glm-5.3'):
    request={'model':model,'max_tokens':4096,
             'messages':[{'role':'user','content':'Return action finish and summary compatibility smoke. No other work is requested.'}],
             'tools':[{'name':'emit_json','description':'Return only the requested JSON object.',
                       'input_schema':{'type':'object','properties':{'action':{'type':'string'},'summary':{'type':'string'}},
                                       'required':['action','summary'],'additionalProperties':False}}],
             'tool_choice':{'type':'tool','name':'emit_json'},'extra_body':{'reasoning_effort':'high'}}
    start=time.time()
    try:
        response=client.messages.create(**request);body=response.model_dump(mode='json')
        tools=[b for b in body['content'] if b['type']=='tool_use']
        ok=body.get('stop_reason')=='tool_use' and len(tools)==1 and isinstance(tools[0]['input'],dict)
        rows.append({'model':model,'request':request,'response':body,'seconds':time.time()-start,
                     'status':'pass' if ok else 'invalid_or_truncated'})
    except Exception as error:
        rows.append({'model':model,'request':request,'seconds':time.time()-start,'status':'error',
                     'error_type':type(error).__name__,'status_code':getattr(error,'status_code',None)})
output=ROOT/'GLM_EXPLICIT_HIGH_SMOKE.json'
with output.open('x') as h:json.dump({'rows':rows,'boundary':'Small compatibility requests only; no refinement effectiveness or effort-equivalence conclusion.'},h,indent=2)
print(json.dumps([{'model':r['model'],'status':r['status'],'seconds':r['seconds'],
                   'usage':r.get('response',{}).get('usage')} for r in rows]),flush=True)
