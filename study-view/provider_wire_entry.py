"""Preserve actual Anthropic replies before structured-output parsing can fail."""
import argparse
import importlib.util
import json
import os
import sys
import time
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source-root', required=True, type=Path)
    parser.add_argument('--architecture', choices=('x86','arm64'), default='x86')
    parser.add_argument('arguments', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    source = args.source_root
    if args.architecture == 'arm64':
        spec = importlib.util.spec_from_file_location('recorded_arm_adapter', Path(__file__).with_name('arm64_entry.py'))
        adapter = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(adapter)
        adapter.install(source)
    sys.path.insert(0, str(source))
    from src.llm import anthropic_chat_model
    original = anthropic_chat_model.Anthropic
    output = Path(os.environ['SEMWEEVER_PROVIDER_WIRE_LOG'])
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        assert output.stat().st_size == 0, 'Never overwrite a received wire response'
    else:
        output.open('x').close()
    def append(value):
        with output.open('a') as handle:
            handle.write(json.dumps(value,ensure_ascii=False)+'\n')
            handle.flush()
            os.fsync(handle.fileno())
    def client_factory(**kwargs):
        client = original(**kwargs)
        create = client.messages.create
        def recorded_create(**request):
            append({'event':'request','timestamp':time.time(),'body':request})
            try:
                response = create(**request)
            except Exception as error:
                append({'event':'transport_error','timestamp':time.time(),
                        'error_type':type(error).__name__,'status_code':getattr(error,'status_code',None)})
                raise
            append({'event':'response','timestamp':time.time(),'body':response.model_dump(mode='json')})
            return response
        client.messages.create = recorded_create
        return client
    anthropic_chat_model.Anthropic = client_factory
    script = source/'experiments/knighter/experiment/run_semweaver_treatment_case.py'
    spec = importlib.util.spec_from_file_location('wire_recorded_treatment',script)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    arguments = args.arguments[1:] if args.arguments[:1]==['--'] else args.arguments
    sys.argv = [str(script),*arguments]
    return module.main()


if __name__ == '__main__':
    raise SystemExit(main())
