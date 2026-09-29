"""Complete a metadata-only rejected reply by deterministic offline replay."""
import fcntl
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time
from build_final39_summary import bound
from replay_received_summary_entry import normalized_payload

ROOT = Path(__file__).resolve().parent
PROJECT = ROOT.parent


def read(path): return json.loads(Path(path).read_text())
def sha(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def save(path, value):
    with path.open('x') as handle: json.dump(value, handle, indent=2)


if __name__ == '__main__':
    parent = Path(sys.argv[1]).resolve()
    relative = parent.relative_to(ROOT)
    assert relative.parts[0] == 'e4-focused12'
    outer, plan = read(parent / 'RUN_MANIFEST.json'), read(parent / 'RUN_PLAN.json')
    assert outer['status'] == 'partial' and outer['inputs_unchanged']
    assert all(sha(p) == h for p, h in plan['inputs'].items())
    cfg, progress = read(parent / 'CONFIG.json'), read(parent / 'PROGRESS.json')
    assert plan['model_response_cap'] == 2 and progress['state']['calls'] == 1 and len(progress['rows']) == 1
    bound(progress['best'])
    first = progress['rows'][0]
    assert sha(Path(first['attempt']) / 'RUN_MANIFEST.json') == first['manifest_sha256']
    wire = parent / 'continuation-02/provider_wire.jsonl'
    events = [json.loads(s) for s in wire.read_text().splitlines()]
    request = [e['body'] for e in events if e['event'] == 'request']
    response = [e['body'] for e in events if e['event'] == 'response']
    assert len(request) == len(response) == 1
    assert request[0]['model'] == cfg['profile']['model'] and request[0]['max_tokens'] == 65536
    assert request[0]['extra_body'] == {'reasoning_effort': 'high', 'output_config': {'effort': 'high'}}
    normalized_payload(response[0])
    failed_manifest = parent / 'continuation-02/RUN_MANIFEST.json'
    failed = read(failed_manifest)
    assert not failed['success'] and failed['model_calls_used'] == 0
    assert 'anthropic_json_tool_missing_or_invalid' in failed['error_message']
    source = Path(first['attempt']) / 'SAGenTestChecker.cpp'
    assert sha(source) == failed['candidate_sha256'] == failed['starting_checker_sha256']
    assert sha(parent / 'continuation-02/SAGenTestChecker.cpp') == sha(source)
    lane = PROJECT / 'LLM-Native/SemWeaver/artifacts/external/linux-state-recovery-v41'
    lock = (ROOT / '.profile-cap-spare-kernel.lock').open('a+')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    output = ROOT / 'profile-repairs' / relative
    output.mkdir(parents=True, exist_ok=False)
    save(output / 'PREFIX_PROGRESS.json', progress)
    cfg.update(linux_dir=str(lane), prefix_parent=str(output), prefix_calls=1,
               prefix_attempts=[first['attempt']], response_replay_wire=str(wire),
               response_replay_candidate_sha256=sha(source))
    save(output / 'CONFIG.json', cfg)
    paths = [Path(__file__), ROOT / 'run_received_summary_repair.py', ROOT / 'replay_received_summary_entry.py',
             ROOT / 'run_profile_cap_repair.py', output / 'CONFIG.json', output / 'PREFIX_PROGRESS.json',
             parent / 'RUN_MANIFEST.json', parent / 'PROGRESS.json', wire, failed_manifest, source]
    inputs = {**plan['inputs'], **{str(p): sha(p) for p in paths}}
    receipt = {**plan, 'inputs': inputs, 'parent': str(parent),
               'operational_repair': 'Replay already received second edit with empty explanatory summary only. Original action/snippets unchanged; no network, model call or manual checker edit.',
               'received_reply_count': 2, 'new_model_calls': 0}
    save(output / 'RUN_PLAN.json', receipt)
    save(output / 'BUDGET.json', {'deadline_epoch': plan['deadline_epoch'], 'max_requests': 0,
                                'dispatched_requests': 0, 'dispatches': [], 'offline_received_reply_replay': True})
    command = ['docker', 'run', '--rm', '--network', 'none', '--name', 'semweaver-received-summary-' + cfg['case_id'][:3],
               '-v', f'{PROJECT}:{PROJECT}', '-v', f'{PROJECT/"LLM-Native"}:/work',
               '-v', '/anonymous/home/.glm-key:/run/private/model-key:ro',
               '-w', '/work/SemWeaver-v43', '-e', 'PYTHONPATH=/work/SemWeaver-v43',
               '-e', 'PATH=/usr/lib/llvm-18/bin:/usr/bin:/bin', '-e', 'GIT_CONFIG_COUNT=2',
               '-e', 'GIT_CONFIG_KEY_0=safe.directory', '-e', f'GIT_CONFIG_VALUE_0={lane}',
               '-e', 'GIT_CONFIG_KEY_1=safe.directory', '-e', f'GIT_CONFIG_VALUE_1={PROJECT/"LLM-Native/SemWeaver-v43"}',
               'sha256:2ddb870c297ffbb9b30b901f84da5542a8ae37eb8d6ce332eae1657fdd0014ec',
               '/work/SemWeaver/.venv-dev/bin/python', str(ROOT / 'run_received_summary_repair.py'), str(output / 'CONFIG.json')]
    with (output / 'container.stdout').open('x') as stdout, (output / 'container.stderr').open('x') as stderr:
        code = subprocess.run(command, stdout=stdout, stderr=stderr,
                              timeout=max(1, plan['deadline_epoch']-time.time())).returncode
    result = output / 'RESULT.json'
    unchanged = all(sha(p) == h for p, h in inputs.items())
    save(output / 'RUN_MANIFEST.json', {**receipt, 'status': 'completed' if code == 0 and unchanged and result.exists() else 'partial',
                                      'return_code': code, 'inputs_unchanged': unchanged,
                                      'result_sha256': sha(result) if result.exists() else None, 'finished_at': time.time()})
    print(json.dumps({'case': cfg['case_id'], 'return_code': code, 'new_model_calls': 0, 'received_replies': 2}), flush=True)
