"""English reading copies of frozen main prompts; originals remain unchanged."""
import argparse
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parent
GLOSSARY = {
    '未提供': 'not provided', '结构审查通过': 'Structural review passed',
    '但发现以下提示': 'with the following advisory notes',
    '代码引入了': 'code introduces', '路径敏感相关符号或说明': 'path-sensitive symbols or claims',
    '但实现里没有对应的状态读写': 'but the implementation lacks corresponding state reads/writes',
    '约束传播或': 'constraint propagation or', '绑定': 'binding',
    '如果当前路线本来就是': 'If the intended approach is', '实参': 'arguments', '建模': 'modeling',
    '应删掉这类': 'remove such', '话术': 'claims',
    '只有显式声明了状态建模却完全未落地时才应继续升级处理': 'Escalate only when state modeling is explicitly claimed but entirely unimplemented',
    '固定噪声': 'fixed-side noise', '要求候选发生实际变化': 'requires an actual candidate change',
    '但最终工件与基线相同': 'but the final artifact equals the baseline',
    '编译失败': 'Compilation failed', '个错误': 'errors', '个警告': 'warnings',
    '未产生实际修改': 'produced no actual change', '但模型预算内未修改工件': 'but no artifact change occurred within the model budget',
    '第': 'number', '个': '', '的': '', '未命中': 'did not match',
    '模型返回了': 'The model returned', '但没有提供任何有效': 'but supplied no valid',
    '代码含有': 'code contains', '简化实现': 'simplified implementation',
    '一类说明注释': 'style explanatory comments', '仅作提示': 'This is advisory only',
    '真正阻断仍以空壳': 'Blocking still depends on stub',
    '空状态建模或': 'empty state modeling or', '黑名单式报告为准': 'blacklist-style reporting',
    '命中': 'matched', '次': 'times', '不唯一': 'not unique',
    '生成产物结构审查未通过': 'Generated-artifact structural review failed',
    '存在语义空壳': 'semantic stubs are present', '占位实现或高风险结构问题': 'placeholder implementations or high-risk structural issues',
    '结构审查未通过': 'Structural review failed', '发现以下高风险问题': 'the following high-risk issues were found',
    '试图从': 'attempts to use', '恢复固定缓冲区容量': 'to recover a fixed buffer capacity',
    '但没有先': 'without first applying', '也没有沿': 'or traversing', '回溯': 'backwards',
    '对': 'For', '这类': 'such', '目的地': 'destinations', '这通常会直接': 'this commonly yields',
    '请在保留漏洞语义目标的前提下做定点修复': 'Apply a localized repair while preserving the intended vulnerability semantics',
    '不要通过删逻辑或改名绕过审查': 'Do not bypass review by deleting logic or renaming identifiers',
}


def translated(text):
    value = text
    for old in sorted(GLOSSARY, key=len, reverse=True):
        value = value.replace(old, GLOSSARY[old])
    for old, new in {'，': ', ', '。': '.', '；': '; ', '：': ': ', '（': '(', '）': ')',
                     '、': ' / ', '“': '"', '”': '"'}.items():
        value = value.replace(old, new)
    unknown = sorted(set(re.findall('[\u4e00-\u9fff]+', value)))
    return value, unknown


def sha(raw): return hashlib.sha256(raw).hexdigest()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    assert not output.exists(), 'Never overwrite an earlier translation snapshot'
    main_raw = (ROOT / 'FINAL39_SUMMARY.json').read_bytes()
    main = json.loads(main_raw)
    assert main['status'] == 'completed_main_baseline39'
    exchanges = []
    for arm in ('native', 'no_internal'):
        for row in main['portfolios'][arm]['rows']:
            result = json.loads((Path(row['cell']) / 'RESULT.json').read_text())
            for attempt in result['rows']:
                path = Path(attempt['attempt']) / 'llm_exchanges.jsonl'
                if not path.exists(): continue
                for index, line in enumerate(path.read_text().splitlines()):
                    item = json.loads(line)
                    assert sha(item['prompt'].encode()) == item['prompt_sha256']
                    assert sha(item['system_prompt'].encode()) == item['system_prompt_sha256']
                    system, missing_system = translated(item['system_prompt'])
                    prompt, missing_prompt = translated(item['prompt'])
                    assert not missing_system + missing_prompt, 'Untranslated prompt fragments: ' + repr(missing_system + missing_prompt)
                    exchanges.append({'case_id': row['case_id'], 'arm': arm, 'original_exchange': str(path),
                                      'exchange_line': index+1, 'original_exchange_file_sha256': sha(path.read_bytes()),
                                      'original_prompt_sha256': item['prompt_sha256'],
                                      'original_system_prompt_sha256': item['system_prompt_sha256'],
                                      'system_prompt_english': system, 'prompt_english': prompt,
                                      'translation_scope': 'Human-authored glossary translation of Chinese runtime feedback, reading companion only; original experimental prompt remains authoritative.'})
    assert len(exchanges) == 283
    output.mkdir(parents=True)
    (output / 'PROMPTS.jsonl').write_text(''.join(json.dumps(r, ensure_ascii=False)+'\n' for r in exchanges))
    (output / 'GLOSSARY.json').write_text(json.dumps(GLOSSARY, ensure_ascii=False, indent=2)+'\n')
    (output / 'README.md').write_text(
        '# English prompt reading companions: main39\n\n'
        'These283 copies translate Chinese runtime-feedback fragments through an\n'
        'explicit human-authored glossary. They are reading companions, not the\n'
        'prompts used to generate new experimental outcomes. Original messages,\n'
        'code and source hashes remain authoritative and unchanged. No model call\n'
        'was made for this translation. Auxiliary/model/raw-wire prompts need\n'
        'their separate final coverage pass; this snapshot is main-only.\n')
    manifest = {'status': 'complete_main_prompt_reading_companions_only', 'exchanges': len(exchanges),
                'source_main_sha256': sha(main_raw), 'generator_sha256': sha(Path(__file__).read_bytes()),
                'files': {p.name: sha(p.read_bytes()) for p in output.iterdir() if p.is_file()},
                'new_model_calls': 0, 'boundary': 'Not final full-artifact translation coverage or scientific readiness.'}
    (output / 'MANIFEST.json').write_text(json.dumps(manifest, indent=2)+'\n')
    print(json.dumps({'exchanges': len(exchanges), 'untranslated_chinese_fragments': 0, 'new_model_calls': 0}))
