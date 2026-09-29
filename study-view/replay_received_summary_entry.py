"""Replay an already received edit; only absent explanatory summary becomes ''."""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import runpy
import subprocess
import sys


def normalized_payload(reply):
    assert reply['stop_reason'] == 'tool_use'
    tools = [b for b in reply['content'] if b['type'] == 'tool_use']
    assert len(tools) == 1 and tools[0]['name'] == 'emit_json'
    raw = tools[0]['input']
    assert isinstance(raw, dict) and raw.get('action') == 'apply_patch'
    assert 'summary' not in raw and 'path' not in raw
    assert isinstance(raw.get('edits'), list) and raw['edits']
    assert all(set(e) == {'old_snippet', 'new_snippet'} and
               all(isinstance(v, str) for v in e.values()) for e in raw['edits'])
    value = copy.deepcopy(raw)
    value['summary'] = ''
    assert {k: v for k, v in value.items() if k != 'summary'} == raw
    return value


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--source-root', type=Path, required=True)
    parser.add_argument('--recorded-wire', type=Path, required=True)
    parser.add_argument('--candidate-sha', required=True)
    parser.add_argument('arguments', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    arguments = args.arguments[1:] if args.arguments[:1] == ['--'] else args.arguments
    before = Path(arguments[arguments.index('--starting-candidate') + 1])
    assert hashlib.sha256(before.read_bytes()).hexdigest() == args.candidate_sha
    output = Path(arguments[arguments.index('--output-dir') + 1])
    events = [json.loads(s) for s in args.recorded_wire.read_text().splitlines()]
    requests = [e['body'] for e in events if e['event'] == 'request']
    replies = [e['body'] for e in events if e['event'] == 'response']
    assert len(requests) == len(replies) == 1
    request, reply = requests[0], replies[0]
    decision = normalized_payload(reply)
    sys.path.insert(0, str(args.source_root))
    from src.llm import anthropic_chat_model as adapter
    from src.llm.usage import normalize_usage
    from langchain_core.messages import AIMessage
    from langchain_core.outputs import ChatGeneration, ChatResult
    used = False
    def replay(self, messages, stop=None, run_manager=None, **kwargs):
        global used
        assert not used and self.model == request['model'] and self.max_tokens == request['max_tokens'] == 65536
        assert kwargs.get('response_format') == {'type': 'json_object'}
        used = True
        output.mkdir(parents=True, exist_ok=True)
        with (output / 'provider_wire.jsonl').open('xb') as stream:
            stream.write(args.recorded_wire.read_bytes())
        certificate = {'status': 'received_reply_metadata_normalization_no_new_inference',
                       'original_wire': str(args.recorded_wire),
                       'original_wire_sha256': hashlib.sha256(args.recorded_wire.read_bytes()).hexdigest(),
                       'original_reply_id': reply.get('id'), 'original_candidate_sha256': args.candidate_sha,
                       'normalization': 'Absent non-operative summary set to empty string; action and all three edits unchanged.',
                       'new_model_calls': 0, 'new_transport_requests': 0,
                       'exchange_boundary': 'Any local prompt serialization belongs to deterministic replay, not a new model request. Original request/reply is provider_wire.jsonl.'}
        with (output / 'REPLAY_NORMALIZATION.json').open('x') as stream:
            json.dump(certificate, stream, indent=2)
        usage = normalize_usage(reply['usage'], model=self.model)
        message = AIMessage(content=json.dumps(decision), response_metadata={
            'model_name': self.model, 'usage': usage, 'json_wire_mode': 'received_reply_missing_summary_replay',
            'original_wire': str(args.recorded_wire), 'new_inference': False},
            usage_metadata={'input_tokens': usage['prompt_tokens'], 'output_tokens': usage['completion_tokens'],
                            'total_tokens': usage['total_tokens']})
        return ChatResult(generations=[ChatGeneration(message=message)])
    adapter.AnthropicMessagesChatModel._generate = replay
    def forbid(*args, **kwargs):
        raise RuntimeError('Network/model client forbidden during received-reply replay')
    adapter.Anthropic = forbid
    original_popen = subprocess.Popen
    def popen(command, *positional, **kwargs):
        if isinstance(command, list) and len(command) > 1 and Path(command[1]).name == 'run_checker_trace_probe.py':
            command = [command[0], str(Path(__file__).with_name('adaptive_trace_probe.py')),
                       '--source-root', str(args.source_root), '--', *command[2:]]
        return original_popen(command, *positional, **kwargs)
    subprocess.Popen = popen
    script = args.source_root / 'experiments/knighter/experiment/run_semweaver_treatment_case.py'
    sys.argv = [str(script), *arguments]
    runpy.run_path(str(script), run_name='__main__')
