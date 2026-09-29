"""Charge an actual thinking-only first reply and use its one remaining reply.

No fabricated tool action or edit. Original interruption stays untouched; the
derived failure receipt is explicitly identified and bound to the raw response.
"""
import ast
from pathlib import Path

ROOT = Path(__file__).resolve().parent


if __name__ == '__main__':
    original = ROOT / 'launch_received_format_prefix.py'
    source = original.read_text()
    begin = source.index("    tools=[b for b in replies[0]['content']")
    end = source.index("    output=ROOT/'profile-repairs'/relative", begin)
    replacement = '''    assert replies[0]['stop_reason']=='max_tokens'
    assert replies[0]['content'] and all(b['type']=='thinking' for b in replies[0]['content'])
    assert not (original/'RUN_MANIFEST.json').exists() and not (original/'llm_exchanges.jsonl').exists()
    interrupted=original/'INFRASTRUCTURE_INTERRUPTION.json'
    interruption=read(interrupted)
    assert interruption['reason']=='model_output_limit_exhausted' and interruption['same_format_attempts']==1
    assert interruption['case_id']==cfg['case_id'] and interruption['model']==cfg['profile']['model']
    treatment=PROJECT/'LLM-Native/SemWeaver-v43/experiments/knighter/experiment/run_semweaver_treatment_case.py'
    tree=ast.parse(treatment.read_text())
    function=next(n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name=='treatment_method')
    method_namespace={}
    exec(compile(ast.Module(body=[function],type_ignores=[]),str(treatment)+'[method-only]', 'exec'),method_namespace)
    failed_record={'method':method_namespace['treatment_method'](cfg['arm'],cfg['objective']),
                   'objective':cfg['objective'],'case_id':cfg['case_id'],'evidence_mode':cfg['arm']}
'''
    source = source[:begin] + replacement + source[end:]
    changes = {
        'import fcntl\n': 'import fcntl\nimport ast\n',
        "manifest={'model':cfg['profile']['model']":
        "manifest={**failed_record,'receipt_kind':'derived_received_output_limit_certificate','model':cfg['profile']['model']",
        "'latest_failure_title':'received_model_output_contract_error'":
        "'latest_failure_title':'received_thinking_only_output_limit'",
        "'latest_failure_text':'The received emit_json input omitted the required action field. No edit was applied. Return a complete decision object under the original contract.'":
        "'latest_failure_text':'The first received response exhausted its65536-token output ceiling with thinking only. No detector edit was emitted or applied. One received reply remains under the unchanged two-reply budget; return the required complete decision object.'",
        "'failure_type':'missing_required_action_in_received_tool_input'":
        "'failure_type':'thinking_only_received_output_ceiling_exhaustion'",
        'wire,failed_manifest,parent/': 'wire,interrupted,treatment,parent/',
        "paths=[Path(__file__),ROOT/'run_mapped_high_received_prefix.py'":
        "paths=[Path(__file__),ROOT/'launch_received_format_prefix.py',ROOT/'run_mapped_high_received_prefix.py'",
        "'operational_repair':'Actual invalid-format reply charged; one remaining reply, same mapped high/profile/budgets; no manual edit.'":
        "'operational_repair':'Actual thinking-only first reply charged; one remaining reply under unchanged mapped-high/profile/cap; original interruption and raw usage preserved, no inferred edit.'",
    }
    for old, new in changes.items():
        assert source.count(old) == 1, 'Received-prefix launcher drift'
        source = source.replace(old, new)
    exec(compile(source, str(original) + '[thinking-only-received-prefix]', 'exec'),
         {'__name__': '__main__', '__file__': str(Path(__file__).resolve())})
