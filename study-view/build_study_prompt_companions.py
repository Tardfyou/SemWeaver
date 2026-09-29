"""English reading views of actual canonical study prompts, never new inference.

Anthropic raw requests take precedence over a deterministic replay's local
serialization. Final export refuses incomplete auxiliary matrices or unknown
Chinese fragments. --inspect-completed is explicitly a non-release preflight.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path

import build_english_prompt_companions as language
from build_auxiliary_summary import require_complete

ROOT = Path(__file__).resolve().parent
PROJECT = ROOT.parent
DATA = PROJECT / 'LLM-Native/artifacts/fse_revision'
language.GLOSSARY.update({
    '仍是空壳': 'remains a stub', '代码声明了': 'code declares',
    '但没有任何实际读取': 'but contains no actual reads', '写入或状态转移': 'writes or state transitions',
    '如果': 'if', '说明状态': 'state claim', '附加提示': 'additional advisory notes', '非阻断': 'non-blocking',
    '说明状态建模仍是空壳': 'state modeling remains a stub', '检测在': 'checks whether in',
    '代码中': 'code', '某个对象通过容器': 'an object through a container', '链表删除': 'linked-list deletion',
    '如': 'for example', '被释放或失效后': 'after being freed or invalidated',
    '仍存在与被删节点别名相等的指针在后续路径中被当作有效对象使用': 'a pointer aliasing the removed node is still treated as a valid object on a later path',
    '且删除点附近缺少将别名显式置空或等效': 'and near the removal site there is no explicit alias nulling or equivalent',
    '屏障的模式': 'barrier pattern', '查询在用': 'The query uses', '推断': 'to infer', '安全条件': 'safety conditions',
    '建议改为显式建模': 'Prefer explicit modeling of', '参数和控制条件': 'arguments and control conditions',
    '查询使用': 'The query uses', '近似建模': 'to approximately model', '边界条件': 'boundary conditions',
    '这通常不是稳定的语义约束': 'This is generally not a stable semantic constraint',
    '的相对路径可能直接是': 'may have a relative path of simply', '不带前导目录': 'without a leading directory',
    '单独使用': 'Using only', '可能漏掉根目录下的目标文件': 'may miss the target file at the root',
    '查询使用行号先后关系近似': 'The query approximates', '支配关系': 'dominance using line-number order',
    '这在代码重排': 'This is unstable under code reordering', '宏展开或跨语句条件下不稳定': 'macro expansion or conditions spanning statements',
    '无': 'none', '查询语法检查失败': 'Query syntax check failed', '关键诊断': 'Key diagnostics',
    '当前查询': 'Current query', '修复建议': 'Repair advice',
    '禁止通过重命名文件或大幅删减逻辑来绕过错误': 'Do not bypass the error by renaming files or substantially deleting logic',
    '应针对报错行附近做定点修复': 'Apply a localized repair near the reported error line', '原始输出': 'Original output',
    '优先直接替换无法解析的类型名': 'First replace an unresolved type name directly',
    '只有连续两次本地修复仍失败时': 'Only after two consecutive local repair failures', '再检索': 'look up',
    '类型或模块': 'the type or module', '只修正无法解析的类型名或导入': 'Correct only unresolved type names or imports',
    '保留原有查询结构和泛化目标': 'Preserve the existing query structure and generalization goal',
    '快速验证通过': 'Quick validation passed', '没有发现语法错误': 'No syntax errors found',
    '补丁已应用': 'Patch applied', '版本备份': 'Version backup', '编译成功': 'Compilation succeeded',
    '输出文件': 'Output file', '文件大小': 'File size', '字节': 'bytes',
})


def read(path): return json.loads(Path(path).read_text())
def sha(raw): return hashlib.sha256(raw).hexdigest()


def texts(content):
    if isinstance(content, str): return content
    assert isinstance(content, list)
    assert all(isinstance(item, dict) and item.get('type') == 'text' and isinstance(item.get('text'), str) for item in content)
    return '\n'.join(item['text'] for item in content)


def request_view(body):
    return texts(body.get('system', '')), [{'role': m['role'], 'content': texts(m['content'])} for m in body['messages']]


def attempt_prompts(path):
    wire = path / 'provider_wire.jsonl'
    log = path / 'llm_exchanges.jsonl'
    if wire.exists():
        raw = wire.read_bytes()
        for index, line in enumerate(raw.decode().splitlines(), 1):
            event = json.loads(line)
            if event['event'] == 'request':
                system, messages = request_view(event['body'])
                yield {'source': str(wire), 'source_sha256': sha(raw), 'source_line': index,
                       'kind': 'actual_recorded_provider_request', 'model': event['body']['model'],
                       'system': system, 'messages': messages,
                       'request_body_sha256': sha(json.dumps(event['body'], sort_keys=True, separators=(',', ':'), ensure_ascii=False).encode())}
    elif log.exists():
        raw = log.read_bytes()
        for index, line in enumerate(raw.decode().splitlines(), 1):
            event = json.loads(line)
            assert sha(event['system_prompt'].encode()) == event['system_prompt_sha256']
            assert sha(event['prompt'].encode()) == event['prompt_sha256']
            yield {'source': str(log), 'source_sha256': sha(raw), 'source_line': index,
                   'kind': 'recorded_normalized_exchange_prompt', 'model': event['model'],
                   'system': event['system_prompt'], 'messages': [{'role': 'user', 'content': event['prompt']}]}


def collect(main, matrix):
    cells = [(f'main39/{arm}/{row["case_id"]}', Path(row['cell']))
             for arm in ('native', 'no_internal') for row in main['portfolios'][arm]['rows']]
    cells += [(f'{group}/{r["model"]}/{r["case_id"]}/{r["parent"]}', Path(r['selected_cell']))
              for group in ('e3', 'e4') for r in matrix[group]['rows'] if r['verified_complete']]
    seen, result = set(), []
    for label, cell in cells:
        for row in read(cell / 'RESULT.json')['rows']:
            path = Path(row['attempt']).resolve()
            if path in seen: continue
            seen.add(path)
            result.extend({'scope': label, **entry} for entry in attempt_prompts(path))
    for row in main['portfolios']['knighter']['rows']:
        if row['baseline_branch'] != 'actual_recorded_refinement': continue
        path = Path(row['cell']) / 'exchanges.jsonl'
        raw = path.read_bytes()
        for index, line in enumerate(raw.decode().splitlines(), 1):
            event = json.loads(line)
            if event['event'] != 'started': continue
            assert sha(event['prompt'].encode()) == event['prompt_sha256']
            result.append({'scope': 'baseline39/'+row['case_id'], 'source': str(path), 'source_sha256': sha(raw),
                           'source_line': index, 'kind': 'recorded_baseline_single_prompt',
                           'system': '', 'messages': [{'role': 'user', 'content': event['prompt']}],
                           'boundary': 'No separate system field is logged; no unrecorded system prompt is invented.'})
    batch = read(DATA / 'generate_only_knighter_gpt6luna_high_v8/BATCH_RESULT.json')
    audit = read(ROOT / 'BASELINE_NATURAL_REUSE_AUDIT.json')
    by_case = {r['case_id']: r['result'] for r in batch['cases']}
    for row in audit['rows']:
        folder = DATA / 'generate_only_knighter_gpt6luna_high_v8' / by_case[row['case_id']]['checker_id'] / 'prompt_history/0'
        candidates = [p for p in folder.glob('check_report-*.md') if sha(p.read_bytes()) == row['prompt_sha256']]
        assert len(candidates) == 1 and row['reuse_eligible']
        path = candidates[0]
        result.append({'scope': 'baseline-natural-reuse/'+row['case_id'], 'source': str(path),
                       'source_sha256': sha(path.read_bytes()), 'source_line': None, 'kind': 'audited_original_natural_stop_prompt',
                       'system': '', 'messages': [{'role': 'user', 'content': path.read_text()}]})
    e2 = DATA / 'e2_imagemagick_development_v8'
    folders = [e2/name for name in ('auto_loop_416_v10', 'auto_loop_416_v10_r2', 'auto_loop_416_v10_r3')]
    selection = read(PROJECT / 'review-revision-20260927/FINAL_SELECTION.json')
    folders += [Path(r['run']) for r in selection['model_lineage']]
    for folder in folders:
        logs = list(folder.rglob('llm_exchanges.jsonl'))
        for log in logs:
            result.extend({'scope': 'illustration-or-codeql/'+folder.name, **entry} for entry in attempt_prompts(log.parent))
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--inspect-completed', action='store_true')
    args = parser.parse_args()
    main_raw = (ROOT/'FINAL39_SUMMARY.json').read_bytes()
    status_raw = (ROOT/'REMAINING_MATRIX_STATUS.json').read_bytes()
    matrix = json.loads(status_raw)
    if not args.inspect_completed: require_complete(matrix)
    rows = collect(json.loads(main_raw), matrix)
    unknown = Counter()
    for row in rows:
        system, missing = language.translated(row['system'])
        unknown.update(missing)
        messages = []
        for message in row['messages']:
            content, missing = language.translated(message['content'])
            unknown.update(missing)
            messages.append({'role': message['role'], 'content': content})
        row['system_english'] = system
        row['messages_english'] = messages
        row['translation_origin'] = 'Assistant-prepared explicit glossary; reading copy, not a new experimental request.'
        row.pop('system'); row.pop('messages')
    if args.inspect_completed:
        print(json.dumps({'status': 'completed-records-language-preflight_only', 'requests': len(rows),
                          'unknown_fragments': dict(unknown), 'publication_ready': False}))
    else:
        assert not unknown, 'Untranslated fragments: '+repr(dict(unknown))
        assert args.output and not args.output.exists()
        args.output.mkdir(parents=True)
        (args.output/'PROMPTS.jsonl').write_text(''.join(json.dumps(r, ensure_ascii=False)+'\n' for r in rows))
        (args.output/'GLOSSARY.json').write_text(json.dumps(language.GLOSSARY, ensure_ascii=False, indent=2)+'\n')
        (args.output/'README.md').write_text(
            '# English prompt reading companions\n\nCanonical main39, repeated ablation, focused model conditions, actual baseline histories,\n'
            'the automated motivating illustration and CodeQL decodes are included. Actual\n'
            'Anthropic requests take precedence over local replay serializations. Originals\n'
            'remain authoritative; translations are not prompts for new scored executions.\n'
            'Historical superseded trial output stays in the raw archive. Upstream prompt\n'
            'templates are included with source; unavailable upstream generation exchanges\n'
            'are not invented. No model call was made to generate these reading views.\n')
        manifest = {'status': 'complete_declared_canonical_prompt_reading_views', 'requests': len(rows),
                    'bindings': {str(ROOT/'FINAL39_SUMMARY.json'): sha(main_raw),
                                 'auxiliary_snapshot_sha256': sha(status_raw), 'generator_sha256': sha(Path(__file__).read_bytes()),
                                 'glossary_input_sha256': sha(Path(language.__file__).read_bytes())},
                    'files': {p.name: sha(p.read_bytes()) for p in args.output.iterdir() if p.is_file()}, 'new_model_calls': 0}
        (args.output/'MANIFEST.json').write_text(json.dumps(manifest, indent=2)+'\n')
        print(json.dumps({'status': manifest['status'], 'requests': len(rows), 'new_model_calls': 0}))
