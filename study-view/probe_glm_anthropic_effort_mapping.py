"""Bounded protocol diagnostic: Anthropic-native output_config vs flat effort."""
import json
import time
from pathlib import Path
from anthropic import Anthropic

ROOT=Path(__file__).resolve().parent
key=Path('/run/private/model-key');assert key.stat().st_mode&0o077==0
client=Anthropic(auth_token=key.read_text().strip(),base_url='https://open.bigmodel.cn/api/anthropic',timeout=180,max_retries=0)
base={'model':'glm-5.3-flash','max_tokens':4096,
      'messages':[{'role':'user','content':'Compute a_20 modulo1009 where a_0=7 and a_(n+1)=(3*a_n+2) mod1009. Return action finish, a short summary with the numerical answer, and no other work.'}],
      'tools':[{'name':'emit_json','description':'Return the numerical answer in the requested object.',
                'input_schema':{'type':'object','properties':{'action':{'type':'string'},'summary':{'type':'string'}},'required':['action','summary'],'additionalProperties':False}}],
      'tool_choice':{'type':'tool','name':'emit_json'}}
rows=[]
for effort in ('low','high'):
    request={**base,'extra_body':{'output_config':{'effort':effort}}};start=time.time()
    try:
        response=client.messages.create(**request).model_dump(mode='json')
        rows.append({'request':request,'response':response,'seconds':time.time()-start,'status':'returned'})
    except Exception as error:
        rows.append({'request':request,'seconds':time.time()-start,'status':'error','error_type':type(error).__name__,'status_code':getattr(error,'status_code',None)})
with (ROOT/'GLM_ANTHROPIC_EFFORT_MAPPING_SMOKE.json').open('x') as h:
    json.dump({'rows':rows,'boundary':'Two bounded compatibility/effort-shape probes, not sample refinement or proof of cross-model equal compute.'},h,indent=2)
print(json.dumps([{'effort':r['request']['extra_body'],'status':r['status'],'seconds':r['seconds'],
                   'stop':r.get('response',{}).get('stop_reason'),'usage':r.get('response',{}).get('usage')} for r in rows]),flush=True)
