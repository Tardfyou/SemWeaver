"""Offline byte, paired-execution and aggregate checks for the final study.

Use --source-project only for a private staging check. Public packages use
project/ plus ORIGINAL_PUBLIC_HASHES.json; no machine-specific mounts are needed.
This is an audit of recorded executions, not a new target-label adjudication.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import statistics


class AuditError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise AuditError(message)


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def read(path):
    return json.loads(Path(path).read_text())


def metrics(rows):
    n = len(rows)
    tp = sum(r['vulnerable_alerts'] > 0 for r in rows)
    fp = sum(r['fixed_alerts'] > 0 for r in rows)
    return dict(cases=n, TP=tp, FP=fp, FN=n-tp, TN=n-fp,
                PDS=sum(r['vulnerable_alerts'] > 0 and r['fixed_alerts'] == 0 for r in rows),
                precision=tp/(tp+fp) if tp+fp else None,
                recall=tp/n if n else None,
                F1=2*tp/(n+tp+fp) if n+tp+fp else None,
                fixed_warning_reports=sum(r['fixed_alerts'] for r in rows))


def same_metrics(actual, expected):
    require(set(actual) == set(expected), 'Metric schema mismatch')
    for key, value in expected.items():
        observed = actual[key]
        if isinstance(value, float):
            require(isinstance(observed, (int, float)) and
                    math.isfinite(observed) and math.isclose(observed, value, abs_tol=1e-12),
                    f'Metric mismatch: {key}')
        else:
            require(observed == value, f'Metric mismatch: {key}')


def direction(native, control):
    nv, nf = native
    cv, cf = control
    if nv > 0 and cv == 0:
        return 'positive_warning_recovery'
    if nv > 0 and cv > 0 and nf < cf:
        return 'positive_fixed_warning_reduction'
    if cv > 0 and nv == 0:
        return 'negative_lost_version_hit'
    if nv > 0 and cv > 0 and nf > cf:
        return 'negative_more_fixed_warnings'
    return 'tie_or_other'


class Auditor:
    def __init__(self, root, source_project=None):
        self.root = Path(root).resolve()
        self.source = Path(source_project).resolve() if source_project else None
        self.project = self.source or self.root / 'project'
        self.phase = self.project / 'final-native-20260928'
        self.crosswalk = {}
        if not self.source:
            data = read(self.root / 'ORIGINAL_PUBLIC_HASHES.json')
            for row in data['files']:
                require(row['path'] not in self.crosswalk, 'Duplicate crosswalk path')
                self.crosswalk[row['path']] = row
        self.pairs = 0
        self.cells = set()
        self.starts = {}
        self.raw_pairs = {}
        self.baseline_emitted_pairs = 0

    def path(self, recorded):
        name = str(recorded)
        prefixes = [('/artifact/project/', self.project),
                    ('/artifact/work/', self.project / 'LLM-Native'),
                    ('/work/', self.project / 'LLM-Native')]
        if self.source:
            prefixes.insert(0, (str(self.source) + '/', self.source))
        for prefix, base in prefixes:
            if name.startswith(prefix):
                candidate = base / name[len(prefix):]
                break
        else:
            candidate = self.root / name
        candidate = candidate.resolve()
        allowed = self.project if self.source else self.root
        require(candidate.is_relative_to(allowed), 'Reference escapes artifact')
        return candidate

    def bound_file(self, recorded, original_sha):
        path = self.path(recorded)
        require(path.is_file(), f'Missing bound file: {path.name}')
        actual = digest(path)
        if self.source:
            require(actual == original_sha, f'Original digest mismatch: {path.name}')
        else:
            relative = path.relative_to(self.root).as_posix()
            row = self.crosswalk.get(relative)
            require(row is not None and row['original_sha256'] == original_sha and
                    row['public_sha256'] == actual, f'Public digest mismatch: {relative}')
        return path

    def original_digest(self, path):
        if self.source:
            return digest(path)
        relative = Path(path).relative_to(self.root).as_posix()
        row = self.crosswalk.get(relative)
        require(row is not None, 'Missing original/public digest crosswalk')
        self.bound_file(str(path), row['original_sha256'])
        return row['original_sha256']

    def pair(self, row):
        for key in ('vulnerable_alerts', 'fixed_alerts'):
            require(type(row[key]) is int and row[key] >= 0, 'Invalid scored warning count')
        self.bound_file(row['candidate'], row['candidate_sha256'])
        raw = read(self.bound_file(row['origin'], row['result_sha256']))
        require(raw.get('execution_valid') is True and not raw.get('infrastructure_errors')
                and not raw.get('scan_failure_artifacts'), 'Unhealthy scored execution')
        require(raw['candidate_sha256'] == row['candidate_sha256'], 'Wrong paired checker')
        for key in ('vulnerable_alerts', 'fixed_alerts'):
            require(raw[key] == row[key], 'Paired warning count mismatch')
        self.raw_pairs[self.path(row['origin'])] = row
        self.pairs += 1

    def baseline_replay(self, cell, paired, start):
        summaries = list((cell / 'output').glob('*/MATCHED_BASELINE_RESULT.json'))
        require(len(summaries) == 1, 'Missing/ambiguous baseline outer-output summary')
        summary = read(self.bound_file(str(summaries[0]), paired['baseline_summary_sha256']))
        require(summary['execution_valid'] and summary['model_calls_used'] == paired['model_responses'],
                'Baseline outer-output health/count mismatch')
        selected = dict(start)
        seen = {start['candidate_sha256']}
        emitted = []
        for attempt in summary['results']:
            candidate = summaries[0].parent / 'refinements' / f"attempt_{attempt['attempt_id']}.cpp"
            if not candidate.is_file():
                require(not attempt['refined'], 'Missing emitted refined baseline checker')
                continue
            candidate_sha = self.original_digest(candidate)
            if candidate_sha in seen:
                continue
            seen.add(candidate_sha)
            validation = cell / 'paired-replay' / f"attempt-{attempt['attempt_id']}" / 'RESULT.json'
            require(validation.is_file(), 'Missing emitted baseline paired validation')
            raw = read(validation)
            entry = dict(candidate=str(candidate), candidate_sha256=candidate_sha,
                         origin=str(validation), result_sha256=self.original_digest(validation),
                         vulnerable_alerts=raw['vulnerable_alerts'], fixed_alerts=raw['fixed_alerts'])
            self.pair(entry)
            improve = entry['vulnerable_alerts'] > 0 and (
                selected['vulnerable_alerts'] == 0 or entry['fixed_alerts'] < selected['fixed_alerts'])
            emitted.append(dict(attempt_id=attempt['attempt_id'], upstream_refined=attempt['refined'],
                                candidate_sha256=candidate_sha, result_sha256=entry['result_sha256'],
                                vulnerable_alerts=entry['vulnerable_alerts'], fixed_alerts=entry['fixed_alerts'],
                                common_adoption=improve))
            if improve:
                selected = entry
        require(emitted == paired['rows'], 'Baseline emitted pair rows/retention differ from raw outputs')
        require(all(selected[k] == paired['selected'][k] for k in
                    ('candidate_sha256', 'vulnerable_alerts', 'fixed_alerts')), 'Wrong baseline retained output')
        self.baseline_emitted_pairs += len(emitted)

    def cell(self, recorded, cap, model, ceiling):
        cell = self.path(recorded)
        if cell in self.cells:
            return read(cell / 'RESULT.json')
        result = read(cell / 'RESULT.json')
        plan, cfg = read(cell / 'RUN_PLAN.json'), read(cell / 'CONFIG.json')
        require(result['status'] == 'completed', 'Incomplete selected cell')
        require(plan['model_response_cap'] == cap, 'Wrong reply ceiling')
        if 'profile' in cfg:
            require(plan['output_token_ceiling'] == ceiling, 'Wrong output ceiling')
            require(cfg['profile']['model'] == model and cfg['profile']['reasoning_effort'] == 'high',
                    'Wrong selected model configuration')
        else:
            # Historical main receipts have no profile or output field. Check
            # the actual attempt manifests, not fabricated rewritten metadata.
            require(model == 'gpt-6-luna' and ceiling == 16384, 'Wrong main configuration')
        calls = 0
        seen = set()
        for row in result['rows']:
            attempt = self.path(row['attempt'])
            require(attempt not in seen, 'Duplicate consumed attempt')
            seen.add(attempt)
            manifest_path = self.bound_file(str(row['attempt']) + '/RUN_MANIFEST.json',
                                            row['manifest_sha256'])
            manifest = read(manifest_path)
            self.bound_file(str(row['attempt']) + '/SAGenTestChecker.cpp', manifest['candidate_sha256'])
            require(manifest['model'] == model and manifest['reasoning_effort'] == 'high',
                    'Attempt model drift')
            if manifest.get('max_tokens_per_call') is not None:
                require(manifest['max_tokens_per_call'] == ceiling, 'Attempt output ceiling drift')
            require(type(manifest['model_calls_used']) is int and manifest['model_calls_used'] >= 0,
                    'Invalid consumed-reply count')
            calls += manifest['model_calls_used']
            if row['paired_valid']:
                validation = attempt / 'frozen_validation/RESULT.json'
                require(validation.is_file(), 'Missing scored attempt validation')
                v = read(validation)
                require(v['execution_valid'] is True and not v.get('infrastructure_errors') and
                        not v.get('scan_failure_artifacts'), 'Unhealthy attempt execution')
                require(v['candidate_sha256'] == manifest['candidate_sha256'], 'Wrong attempt checker')
                require(all(v[k] == row[k] for k in ('vulnerable_alerts', 'fixed_alerts')),
                        'Attempt counts mismatch')
            elif row.get('vulnerable_alerts') is not None or row.get('fixed_alerts') is not None:
                raise AuditError('Invalid attempt imputed as scored')
            certificate = attempt / 'REPLAY_NORMALIZATION.json'
            if certificate.exists():
                proof = read(certificate)
                require(proof['new_model_calls'] == proof['new_transport_requests'] == 0,
                        'Received reply replay claimed new inference')
                original_wire = self.bound_file(proof['original_wire'], proof['original_wire_sha256'])
                self.bound_file(str(attempt / 'provider_wire.jsonl'), proof['original_wire_sha256'])
                events = [json.loads(line) for line in original_wire.read_text().splitlines()]
                replies = [e['body'] for e in events if e['event'] == 'response']
                require(len(replies) == 1 and replies[0]['id'] == proof['original_reply_id'] and
                        replies[0]['stop_reason'] == 'tool_use', 'Wrong replay response')
                tools = [b for b in replies[0]['content'] if b['type'] == 'tool_use']
                require(len(tools) == 1 and tools[0]['name'] == 'emit_json', 'Ambiguous replay decision')
                decision = tools[0]['input']
                require(decision.get('action') == 'apply_patch' and 'summary' not in decision and
                        'path' not in decision and isinstance(decision.get('edits'), list) and decision['edits'],
                        'Replay inferred or redirected a functional action')
                start_sha = proof['original_candidate_sha256']
                require(manifest['starting_checker_sha256'] == start_sha, 'Wrong replay input checker')
                before_paths = []
                for previous in result['rows']:
                    previous_path = self.path(previous['attempt'])
                    previous_manifest = read(previous_path / 'RUN_MANIFEST.json')
                    if previous_manifest['candidate_sha256'] == start_sha:
                        before_paths.append(previous_path / 'SAGenTestChecker.cpp')
                require(before_paths, 'Missing original replay input')
                before = self.bound_file(str(before_paths[0]), start_sha).read_text()
                for edit in decision['edits']:
                    require(set(edit) == {'old_snippet', 'new_snippet'} and
                            all(isinstance(v, str) for v in edit.values()) and
                            before.count(edit['old_snippet']) == 1, 'Replay edit not uniquely model-specified')
                    before = before.replace(edit['old_snippet'], edit['new_snippet'], 1)
                require(before == (attempt / 'SAGenTestChecker.cpp').read_text(),
                        'Scored replay checker differs from received model edits')
        require(calls == result['state']['calls'] and calls <= cap, 'Consumed-reply count mismatch')
        selected = result['selected']
        if selected['origin'] == 'starting_checker':
            start = self.starts[cfg['case_id']]
            require(all(selected[k] == start[k] for k in
                        ('candidate_sha256', 'vulnerable_alerts', 'fixed_alerts')),
                    'Wrong retained starting checker')
            self.pair(start)
        else:
            self.pair(selected)
        self.cells.add(cell)
        return result

    def baseline(self, main):
        reuse = read(self.phase / 'BASELINE_NATURAL_REUSE_AUDIT.json')
        old_batch = self.project / 'LLM-Native/artifacts/fse_revision/generate_only_knighter_gpt6luna_high_v8/BATCH_RESULT.json'
        batch = read(self.bound_file(str(old_batch), reuse['baseline_batch_sha256']))
        originals = {r['case_id']: r['result'] for r in batch['cases']}
        natural = {r['case_id']: r for r in reuse['rows']}
        require(len(natural) == 2, 'Wrong natural-stop reuse denominator')
        branches = Counter()
        for row in main['portfolios']['knighter']['rows']:
            branch = row['baseline_branch']
            branches[branch] += 1
            if branch == 'actual_zero_response_no_report':
                receipt = read(self.path(row['branch_record']))
                require(receipt['model_responses'] == row['model_responses'] == 0,
                        'No-report branch charged/edited unexpectedly')
            elif branch == 'audited_natural_stop_reuse':
                audit, old = natural[row['case_id']], originals[row['case_id']]
                require(audit['reuse_eligible'] and audit['exact_prompt_equivalence'] and
                        audit['recorded_responses'] == row['model_responses'] == 1,
                        'Unproved reuse equivalence')
                require(old['model'] == 'gpt-6-luna' and old['reasoning_effort'] == 'high' and
                        old['model_calls_used'] == 1 and
                        all(not r['refined'] and r['result'] == 'No-FP' for r in old['results']),
                        'Baseline did not naturally terminate')
            elif branch == 'actual_recorded_refinement':
                cell = self.path(row['cell'])
                paired = read(cell / 'PAIRED_RESULT.json')
                health = read(self.bound_file(str(cell / 'exchanges.health.json'), paired['exchange_health_sha256']))
                self.bound_file(str(cell / 'exchanges.jsonl'), health['exchange_log_sha256'])
                require(health['status'] == 'completed' and not health['errors'] and
                        health['consumed_model_responses'] == paired['model_responses'] == row['model_responses'] and
                        row['model_responses'] <= 32, 'Baseline provider failure or wrong response count')
                execution = read(cell / 'EXECUTION_MANIFEST.json')
                require(execution['inputs_unchanged'], 'Baseline input drift')
                self.baseline_replay(cell, paired, self.starts[row['case_id']])
            else:
                raise AuditError('Unknown baseline branch')
        require(branches == {'actual_zero_response_no_report': 24, 'audited_natural_stop_reuse': 2,
                             'actual_recorded_refinement': 13}, 'Wrong baseline branch coverage')

    @staticmethod
    def glm_wire(path, model):
        requests = responses = 0
        for line in path.read_text().splitlines():
            event = json.loads(line)
            if event['event'] == 'request':
                body = event['body']
                require(body['model'] == model and body['max_tokens'] == 65536 and
                        body.get('extra_body', {}).get('reasoning_effort') == 'high' and
                        body.get('extra_body', {}).get('output_config', {}).get('effort') == 'high',
                        'GLM raw request configuration drift')
                requests += 1
            elif event['event'] == 'response':
                responses += 1
        require(requests >= responses == 1, 'Missing/duplicated received GLM reply')

    def main(self):
        main = read(self.phase / 'FINAL39_SUMMARY.json')
        require(main['status'] == 'completed_main_baseline39', 'Main cohort incomplete')
        subjects = [r['case_id'] for r in main['starting']['rows']]
        require(len(subjects) == len(set(subjects)) == 39, 'Wrong main denominator')
        require(set(main['portfolios']) == {'native', 'no_internal', 'knighter'}, 'Missing main method')
        starts = {r['case_id']: r for r in main['starting']['rows']}
        self.starts = starts
        subset = {case for case, r in starts.items() if r['vulnerable_alerts'] > 0 and r['fixed_alerts'] > 0}
        require(len(subset) == 15, 'Changed report-refinement stratum')
        for method, block in {'starting': main['starting'], **main['portfolios']}.items():
            rows = block['rows']
            require(len(rows) == 39 and {r['case_id'] for r in rows} == set(subjects), 'Unequal method subjects')
            same_metrics(block['metrics'], metrics(rows))
            for row in rows:
                self.pair(row)
                if method in ('native', 'no_internal'):
                    cell = self.cell(row['cell'], 32, 'gpt-6-luna', 16384)
                    require(cell['state']['calls'] == row['model_responses'], 'Main reply count mismatch')
                    require(all(cell['selected'][k] == row[k] for k in
                                ('candidate_sha256', 'vulnerable_alerts', 'fixed_alerts')), 'Main selection mismatch')
            if method != 'starting':
                require(block['model_responses'] == sum(r['model_responses'] for r in rows), 'Main cost sum mismatch')
                matched = main['matched15'][method]
                require(len(matched['rows']) == 15 and {r['case_id'] for r in matched['rows']} == subset,
                        'Changed matched subjects')
                same_metrics(matched['metrics'], metrics([r for r in rows if r['case_id'] in subset]))
        self.baseline(main)
        return main

    def auxiliary(self, main):
        aux = read(self.phase / 'AUXILIARY_SUMMARY.json')
        require(aux['status'] == 'completed_auxiliary_matrices', 'Auxiliary matrices incomplete')
        for recorded, sha in aux['bindings'].items():
            self.bound_file(recorded, sha)
        repeated = aux['repeated_ablation']
        require(repeated['status'] == 'complete' and repeated['complete_three_decode_subjects'] == 12 and
                repeated['complete_paired_decodes'] == 36, 'Repeated ablation incomplete')
        subjects = [r['case_id'] for r in repeated['rows']]
        require(len(subjects) == len(set(subjects)) == 12, 'Wrong repeated denominator')
        counts, consistency = Counter(), Counter()
        first = {arm: {r['case_id']: r for r in main['portfolios'][arm]['rows']}
                 for arm in ('native', 'no_internal')}
        for row in repeated['rows']:
            require([r['repeat'] for r in row['repeats']] == [1, 2, 3], 'Wrong decode identities')
            signs = []
            for repeat in row['repeats']:
                alerts = {}
                for arm in ('native', 'no_internal'):
                    entry = repeat[arm]
                    require(entry['complete'] and not entry['integrity_errors'], 'Incomplete repeated cell')
                    result = self.cell(entry['cell'], 32, 'gpt-6-luna', 16384)
                    selected = result['selected']
                    alerts[arm] = [selected['vulnerable_alerts'], selected['fixed_alerts']]
                    require(alerts[arm] == entry['alerts'] and result['state']['calls'] == entry['model_responses'],
                            'Repeated output mismatch')
                    if repeat['repeat'] == 1:
                        require(self.path(entry['cell']) == self.path(first[arm][row['case_id']]['cell']),
                                'First decode not original main output')
                outcome = direction(alerts['native'], alerts['no_internal'])
                require(repeat['paired_complete'] and repeat['outcome'] == outcome, 'Wrong paired direction')
                counts[outcome] += 1
                signs.append(outcome.split('_')[0])
            expected = ('all_positive' if signs == ['positive']*3 else
                        'all_negative' if signs == ['negative']*3 else
                        'all_tie_or_other' if signs == ['tie']*3 else 'mixed')
            require(row['direction_consistency'] == expected, 'Wrong consistency label')
            consistency[expected] += 1
        require(dict(counts) == repeated['decode_outcome_counts'] and
                dict(consistency) == repeated['three_decode_consistency_counts'], 'Wrong repeated aggregate')
        blocks = aux['focused_model_sensitivity']
        require(set(blocks) == {'gpt-6-luna', 'glm-5.3', 'glm-5.3-flash'}, 'Missing model configuration')
        for model, block in blocks.items():
            require(len(block['rows']) == 12 and {r['case_id'] for r in block['rows']} == set(subjects),
                    'Unequal model subjects')
            same_metrics(block['metrics'], metrics(block['rows']))
            for row in block['rows']:
                self.pair(row)
                result = self.cell(row['cell'], 2, model, 65536)
                require(result['state']['calls'] == row['model_responses'], 'Model reply count mismatch')
                require(all(result['selected'][k] == row[k] for k in
                            ('candidate_sha256', 'vulnerable_alerts', 'fixed_alerts')), 'Wrong model selection')
                if model != 'gpt-6-luna':
                    for attempt in result['rows']:
                        self.glm_wire(self.path(attempt['attempt']) / 'provider_wire.jsonl', model)
            require(block['model_responses'] == sum(r['model_responses'] for r in block['rows']), 'Model cost mismatch')
        return aux

    def backend(self):
        """Separate automatic CodeQL case/cost audit; never pooled into CSA39."""
        data = self.project / 'LLM-Native/artifacts/fse_revision'
        e2 = data / 'e2_imagemagick_development_v8'
        summary = read(e2 / 'auto_loop_416_repeats_v10/SUMMARY.json')
        names = ('auto_loop_416_v10', 'auto_loop_416_v10_r2', 'auto_loop_416_v10_r3')
        require(summary['repeats'] == 3 and len(summary['rows']) == 3, 'Wrong CodeQL repeat denominator')
        successes = recoveries = 0
        for index, name in enumerate(names):
            folder = e2 / name
            self.bound_file(str(folder / 'INPUT_MANIFEST.json'), summary['input_manifest_sha256'])
            row = summary['rows'][index]
            result = read(self.bound_file(str(folder / 'RUN_RESULT.json'), row['run_result_sha256']))
            require(result['model'] == 'gpt-6-luna' and result['reasoning_effort'] == 'high' and
                    result['total_model_calls'] == row['model_calls'], 'CodeQL model/cost drift')
            initial = read(self.bound_file(str(folder / 'initial_pair/RESULT.json'), result['initial_pair_result_sha256']))
            require(initial['execution_valid'] and initial['vulnerable_rows'] == initial['fixed_rows'] == 0 and
                    initial['query_sha256'] == result['initial_query_sha256'], 'Invalid CodeQL starting pair')
            calls = 0
            for attempt in result['rows']:
                number = attempt['round']
                manifest = read(self.bound_file(str(folder / f'round-{number:02d}/RUN_MANIFEST.json'),
                                               attempt['agent_manifest_sha256']))
                pair = read(self.bound_file(str(folder / f'round-{number:02d}-pair/RESULT.json'),
                                           attempt['paired_result_sha256']))
                require(manifest['query_sha256'] == pair['query_sha256'] == attempt['candidate_query_sha256'],
                        'CodeQL candidate mismatch')
                queries = list((folder / f'round-{number:02d}').glob('*.ql'))
                require(len(queries) == 1, 'Missing/ambiguous CodeQL candidate file')
                self.bound_file(str(queries[0]), attempt['candidate_query_sha256'])
                require(manifest['llm_usage']['call_count'] == attempt['model_calls'], 'CodeQL consumed-reply mismatch')
                calls += attempt['model_calls']
                require(pair['execution_valid'] == attempt['paired_execution_valid'], 'Wrong CodeQL execution health')
                require(all(pair[k] == attempt[k] for k in ('vulnerable_rows', 'fixed_rows')), 'CodeQL counts mismatch')
                if not pair['execution_valid']:
                    require(pair['vulnerable_rows'] is None and pair['fixed_rows'] is None,
                            'Failed CodeQL execution imputed as zero')
            require(calls == result['total_model_calls'] and calls <= result['total_model_call_cap'], 'CodeQL reply budget mismatch')
            best = result.get('best_hit_recovery_with_fixed_noise')
            if best:
                pair = read(self.bound_file(str(folder / f'round-{best["round"]:02d}-pair/RESULT.json'),
                                           best['paired_result_sha256']))
                require(pair['execution_valid'] and pair['vulnerable_rows'] > 0 and
                        pair['query_sha256'] == best['candidate_query_sha256'], 'Invalid retained CodeQL result')
                require((pair['vulnerable_rows'], pair['fixed_rows']) == (row['best_vulnerable_rows'], row['best_fixed_rows']),
                        'CodeQL best-result mismatch')
                recoveries += 1
            successes += bool(result['success'])
        require(successes == summary['strict_pds_repeats'] and recoveries == summary['target_hit_recovered_repeats'],
                'Wrong CodeQL repeat aggregate')
        cost = data / 'backend_cost_20260927_v1'
        result = read(cost / 'RESULT.json')
        plan = read(self.bound_file(str(cost / 'INPUT_MANIFEST.json'), result['input_manifest_sha256']))
        require(result['completed'] == len(result['rows']) == 12 and result['all_execution_valid'], 'Incomplete CodeQL cost replay')
        for side in ('vulnerable', 'fixed'):
            self.bound_file(str(e2 / f'codeql_db/416_{side}/codeql-database.yml'), plan['database_manifest_hashes'][side])
        pairs = {}
        for variant in ('initial', 'refined'):
            elapsed, rss = [], []
            for repeat in (1, 2, 3):
                rows = [r for r in result['rows'] if r['repeat'] == repeat and r['variant'] == variant]
                require(len(rows) == 2 and {r['side'] for r in rows} == {'vulnerable', 'fixed'}, 'Cost pair missing/duplicated')
                root = cost / f'rep-{repeat:02d}-{variant}'
                queries = list(root.glob('*.ql'))
                require(len(queries) == 1, 'Missing cost query')
                self.bound_file(str(queries[0]), plan['query_hashes'][variant])
                self.bound_file(str(root / 'qlpack.yml'), plan['pack_hashes'][variant])
                for row in rows:
                    require(row['execution_valid'] and row['query']['execution_valid'] and row['decode']['execution_valid'],
                            'Invalid cost execution')
                    tuples = read(root / f'{row["side"]}-decode.stdout.log')['#select']['tuples']
                    require(len(tuples) == row['rows'] == row['expected_rows'], 'Cost tuple count mismatch')
                    rss.append(row['query']['max_rss_kib'])
                elapsed.append(sum(r['query']['wall_seconds'] + r['decode']['wall_seconds'] for r in rows))
            expected = dict(paired_seconds=elapsed, median_paired_seconds=statistics.median(elapsed),
                            min_paired_seconds=min(elapsed), max_paired_seconds=max(elapsed), peak_process_rss_mib=max(rss)/1024)
            require(result['summary'][variant] == expected, 'Cost arithmetic mismatch')
            pairs[variant] = expected
        joined = data / 'backend_cost_summary_20260927_v1'
        joined_result = read(joined / 'RESULT.json')
        self.bound_file(str(cost / 'RESULT.json'), joined_result['replay_result_sha256'])
        require(joined_result['replay_summary'] == pairs and joined_result['historical_developer_hours'] is None,
                'Cost summary or labor boundary mismatch')
        for row in joined_result['database_setup']:
            self.bound_file(str(joined / row['log']), row['log_sha256'])
        for row in joined_result['retained_model_runs']:
            self.bound_file(str(e2 / row['run'] / 'RUN_RESULT.json'), row['result_sha256'])
            calls = tokens = 0
            seconds = 0.0
            for item in row['round_manifests']:
                manifest = read(self.bound_file(str(data / item['path']), item['sha256']))
                require(manifest['llm_usage']['call_count'] == item['model_calls'] and
                        manifest['llm_usage']['total_tokens'] == item['total_tokens'] and
                        manifest['elapsed_seconds'] == item['elapsed_seconds'], 'Model-cost receipt mismatch')
                calls += item['model_calls']
                tokens += item['total_tokens']
                seconds += item['elapsed_seconds']
            require((calls, tokens, seconds) == (row['model_calls'], row['total_tokens'], row['agent_round_seconds']),
                    'Model-cost aggregate mismatch')
        footprint = read(data / 'backend_integration_audit_20260927_v1/RESULT.json')
        counts = {}
        for row in footprint['rows']:
            source = self.project / row['path'] if row['category'] == 'external_e2_protocol_and_tests' else self.project / 'LLM-Native/SemWeaver-v22' / row['path']
            path = self.bound_file(str(source), row['sha256'])
            require(len(path.read_text().splitlines()) == row['physical_lines'], 'Footprint line-count mismatch')
            category = counts.setdefault(row['category'], dict(files=0, physical_lines=0))
            category['files'] += 1
            category['physical_lines'] += row['physical_lines']
        require(counts == footprint['summary'] and footprint['historical_developer_hours'] is None, 'Footprint summary mismatch')
        return dict(codeql_subjects=1, decodes=3, clean_decodes=successes, warning_recovery_decodes=recoveries,
                    cost_side_executions=12, developer_hours_measured=False)

    def evidence(self, main):
        initial = read(self.phase / 'INITIAL_EVIDENCE_CATALOG.json')
        subjects = {r['case_id'] for r in main['starting']['rows']}
        require(initial['status'] == 'verified_initial_bundle_catalog' and len(initial['rows']) == 39 and
                {r['case_id'] for r in initial['rows']} == subjects, 'Initial evidence coverage mismatch')
        origins, types, source_prefixes = Counter(), Counter(), Counter()
        for row in initial['rows']:
            for recorded, expected in row['bindings'].items():
                self.bound_file(recorded, expected)
            bundle = read(self.path(row['bundle']))
            local = Counter()
            internal = []
            for record in bundle['records']:
                provenance = record['provenance']
                origin = provenance['origin']
                require(origin in ('analyzer_internal', 'analyzer_output', 'source_derived'), 'Unknown evidence origin')
                origins[origin] += 1
                types[origin, record['type']] += 1
                local[origin] += 1
                if origin == 'source_derived':
                    source_prefixes[provenance.get('artifact', 'unspecified').split(':', 1)[0]] += 1
                if origin == 'analyzer_internal':
                    payload = record['semantic_payload']
                    require(payload['interface'] == 'clang-18:debug.DumpCFG+debug.DumpCallGraph' and
                            payload['output_schema'] == 'semweaver.csa_cfg_snapshot.v1', 'Native interface/schema drift')
                    self.bound_file(payload['raw_output_path'], payload['raw_output_sha256'])
                    internal.append({'id': record['evidence_id'], 'type': record['type'],
                                     'file': record['scope']['file'], 'function': record['scope']['function'],
                                     'interface': payload['interface'], 'schema': payload['output_schema'],
                                     'raw_output_sha256': payload['raw_output_sha256']})
            require(dict(local) == row['origin_counts'] and internal == row['internal_records'], 'Per-case evidence census mismatch')
        require(dict(origins) == initial['origin_counts'] and sum(origins.values()) == initial['records'] == 193,
                'Initial origin-count mismatch')
        require(dict(source_prefixes) == initial['source_derived_artifact_prefix_counts'], 'Source fallback census mismatch')
        expected_types = [{'origin': origin, 'type': kind, 'records': number}
                          for (origin, kind), number in sorted(types.items())]
        require(expected_types == initial['record_types'], 'Native type-count mismatch')
        ledger = read(self.phase / 'NATIVE_EVIDENCE_AVAILABILITY_STATUS.json')
        require(ledger['status'] == 'complete' and ledger['completed_pairs'] == 39 and
                not ledger['integrity_error_attempts'], 'Dynamic availability incomplete')
        selected_attempts = set()
        for row in main['portfolios']['native']['rows']:
            result = read(self.path(row['cell']) / 'RESULT.json')
            selected_attempts.update(self.path(r['attempt']) for r in result['rows'])
        observed, availability = set(), Counter()
        usable = 0
        for row in ledger['rows']:
            attempt = self.path(row['attempt'])
            require(attempt not in observed and not row['integrity_errors'], 'Duplicate/invalid dynamic entry')
            observed.add(attempt)
            for recorded, expected in row['bindings'].items():
                self.bound_file(recorded, expected)
            manifest = read(attempt / 'RUN_MANIFEST.json')
            require(manifest['model_calls_used'] == row['model_responses'], 'Dynamic reply-count mismatch')
            availability[row['status']] += 1
            if row['dynamic_evidence_available']:
                require(row['status'] == manifest['checker_execution_probe_status'] == 'usable_hash_bound_trace',
                        'Unavailable evidence relabeled usable')
                probe = attempt / 'checker_execution_probe'
                result, receipt = read(probe / 'RESULT.json'), read(probe / 'RUN_MANIFEST.json')
                scans = result['scans']
                require(result['execution_health'] == receipt['execution_health'] == 'completed' and
                        result['diagnostic_parity'] and receipt['diagnostic_parity'] and result['trace_usable'] and
                        receipt['inputs_unchanged'] and scans == receipt['scans'] and len(scans) == 4,
                        'Invalid dynamic probe parity/health')
                require({(s['side'], s['mode']) for s in scans} ==
                        {(s, m) for s in ('vulnerable', 'fixed') for m in ('original', 'traced')} and
                        all(s['execution_valid'] and not s.get('trace_capped') for s in scans), 'Capped/partial dynamic probe')
                payload = read(attempt / 'checker_execution_feedback.json')
                require(payload['checker_sha256'] == manifest['starting_checker_sha256'], 'Wrong dynamic feedback checker')
                for side in payload['sides']:
                    self.bound_file(str(probe / side['side'] / 'traced/trace.jsonl'), side['raw_sha256'])
                    scan = next(s for s in scans if s['side'] == side['side'] and s['mode'] == 'traced')
                    require(scan['trace_events'] and scan['trace_sha256'] == side['raw_sha256'], 'Empty/unbound dynamic evidence')
                usable += 1
        require(observed == selected_attempts and len(observed) == ledger['unique_native_attempts'] == 150,
                'Wrong native availability denominator')
        require(dict(availability) == ledger['status_counts'] and usable == ledger['verified_dynamic_available_attempts'],
                'Dynamic availability arithmetic mismatch')
        return {'initial_records': sum(origins.values()), 'origin_counts': dict(origins),
                'native_attempts': len(observed), 'usable_dynamic_attempts': usable}

    def raw_csa_counts(self):
        """Recount logical reports, retaining healthy/logged zero-scan evidence.

        A report's byte hash is not its identity: identical bytes in different
        scan objects/sides still count separately. Zero scans lack HTML by
        design, and depend on the healthy receipt plus retained build/scan log.
        """
        data = self.project / 'LLM-Native/artifacts/fse_revision'
        corrected = read(data / 'scanfix_v26_corrected_summary_v1/RESULT.json')
        origins = {}
        for row in corrected['starting']['rows']:
            recorded = str(row['validation_path'])
            path = self.path(recorded) if recorded.startswith('/') else data / recorded
            self.bound_file(str(path), row['validation_sha256'])
            origins[row['case_id']] = path
        scans = {}
        for origin, row in self.raw_pairs.items():
            root = origin.parent
            if not (root / 'VALIDATION_RUN.log').is_file():
                raw = read(origin)
                alternate = origins.get(raw['case_id'])
                require(alternate is not None, 'Missing raw validation log/origin')
                self.bound_file(str(alternate), row['result_sha256'])
                origin, root = alternate, alternate.parent
            scans[origin.resolve()] = row
        reports = objects = zero_objects = 0
        for origin, row in scans.items():
            raw = read(origin)
            root = origin.parent
            require(raw['execution_valid'] and raw['build_return_code'] == 0 and
                    not raw.get('infrastructure_errors') and not raw.get('scan_failure_artifacts'),
                    'Unhealthy raw CSA scan')
            log = root / 'VALIDATION_RUN.log'
            require(log.is_file() and (root / 'build_logs/build_stdout_1.log').is_file() and
                    (root / 'build_logs/build_stderr_1.log').is_file(), 'Missing raw CSA build/scan logs')
            text = log.read_text()
            for side in ('vulnerable', 'fixed'):
                counts = raw[side + '_object_counts']
                require(counts and all(type(n) is int and n >= 0 for n in counts.values()),
                        'Invalid raw object counts')
                side_reports = set()
                for obj, expected in counts.items():
                    safe = obj.replace('/', '-').removesuffix('.o')
                    require(safe and '/' not in safe and safe not in ('.', '..'), 'Unsafe scan object name')
                    command_lines = [line for line in text.splitlines() if 'Running: ' in line and
                                     f'/{side}/{safe} ' in line and line.rstrip().endswith(' ' + obj)]
                    require(len(command_lines) == 1, 'Missing/duplicated side-object scan log')
                    files = set((root / side / safe).rglob('report-*.html'))
                    require(len(files) == expected, f'Raw CSA report count mismatch: {raw["case_id"]}/{side}/{safe}')
                    require(not side_reports.intersection(files), 'Overlapping logical scan objects')
                    side_reports.update(files)
                    objects += 1
                    zero_objects += expected == 0
                require(set((root / side).rglob('report-*.html')) == side_reports,
                        'Unattributed raw CSA reports')
                require(len(side_reports) == raw[side + '_alerts'] == row[side + '_alerts'],
                        'Raw CSA side aggregate mismatch')
                reports += len(side_reports)
        return dict(distinct_validation_origins=len(scans), logical_report_files=reports,
                    object_side_scans=objects, healthy_logged_zero_object_scans=zero_objects,
                    baseline_emitted_pairs=self.baseline_emitted_pairs,
                    zero_boundary='Zeros require recorded healthy execution and retained build/scan logs; no raw stdout was invented.')

    def bytes(self):
        require(not self.source, 'Public byte audit needs exported package')
        manifest = read(self.root / 'ARTIFACT_MANIFEST.json')
        require(not any(p.is_symlink() for p in self.root.rglob('*')), 'Public package contains aliases')
        listed = set()
        for row in manifest['files']:
            path = self.path(row['path'])
            require(row['path'] not in listed and not path.is_symlink(), 'Duplicate/linked public file')
            listed.add(row['path'])
            require(path.is_file() and path.stat().st_size == row['size'] and digest(path) == row['sha256'],
                    f'Package byte mismatch: {row["path"]}')
        actual = {p.relative_to(self.root).as_posix() for p in self.root.rglob('*') if p.is_file()}
        require(actual == listed | {'ARTIFACT_MANIFEST.json'} and len(listed) == manifest['file_count'],
                'Unlisted or missing package files')
        for name, row in self.crosswalk.items():
            require(name in listed and digest(self.path(name)) == row['public_sha256'], 'Crosswalk byte mismatch')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('--source-project', type=Path)
    parser.add_argument('--main-only', action='store_true', help='Staging only; never certifies full release')
    args = parser.parse_args()
    require(not args.main_only or args.source_project, 'Main-only audit is private staging only')
    audit = Auditor(args.root, args.source_project)
    if not args.source_project:
        audit.bytes()
    study = audit.main()
    raw_counts = None
    if not args.main_only:
        audit.auxiliary(study)
        audit.backend()
        audit.evidence(study)
        raw_counts = audit.raw_csa_counts()
    print(json.dumps({'status': 'verified_main_staging_only' if args.main_only else 'verified_recorded_study',
                      'verified_paired_records': audit.pairs, 'verified_refinement_cells': len(audit.cells),
                      'raw_csa_recount': raw_counts,
                      'publication_readiness': 'Not certified: paper, other backends, privacy and independent review are separate gates.'}))


if __name__ == '__main__':
    main()
