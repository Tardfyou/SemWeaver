"""Explicit high effort in both GLM-flat and Anthropic-native parameter shapes."""
from pathlib import Path

if __name__=='__main__':
    original=Path(__file__).with_name('glm_high_pipeline_entry.py')
    source=original.read_text()
    old="request['extra_body'] = {'reasoning_effort': 'high'}"
    new="request['extra_body'] = {'reasoning_effort': 'high', 'output_config': {'effort': 'high'}}"
    assert source.count(old)==1
    exec(compile(source.replace(old,new),str(original)+'[native-effort-shape]','exec'),
         {'__name__':'__main__','__file__':str(Path(__file__).resolve())})
