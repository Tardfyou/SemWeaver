"""Export/privacy regressions; no empirical files are modified."""
import json
import os
from pathlib import Path
import tempfile
import unittest
import subprocess

from build_final_artifact import CurrentPackage, PROJECT, describe, corrected_starting_roots, verify_export_inputs, source_state
from verify_final_artifact import Auditor, AuditError
from compressed_artifact import pack, unpack


class ExportTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name) / 'package'
        self.root.mkdir()
        self.package = CurrentPackage(self.root)

    def tearDown(self):
        self.temp.cleanup()

    def finish(self):
        records = list(self.package.crosswalk)
        self.package.put('ORIGINAL_PUBLIC_HASHES.json', json.dumps({'files': records}).encode())
        self.package.finish({'status': 'synthetic_test_only'})

    def test_public_crosswalk_verifies_redacted_receipt(self):
        raw = json.dumps({'candidate': str(PROJECT / 'example.cpp')}).encode()
        self.package.put('project/receipt.json', raw)
        record = self.package.crosswalk[0]
        self.assertNotEqual(record['original_sha256'], record['public_sha256'])
        self.finish()
        auditor = Auditor(self.root)
        auditor.bytes()
        auditor.bound_file('/artifact/project/receipt.json', record['original_sha256'])

    def test_secret_shaped_data_refused_before_write(self):
        synthetic = ('sk-' + 'x'*32).encode()
        with self.assertRaises(AuditError):
            self.package.put('private.txt', synthetic)
        self.assertFalse((self.root / 'private.txt').exists())

    def test_duplicate_export_path_refused(self):
        self.package.put('one.txt', b'first')
        with self.assertRaises(ValueError):
            self.package.put('one.txt', b'second')
        self.assertEqual((self.root / 'one.txt').read_bytes(), b'first')

    def test_author_url_and_identifier_redacted_with_original_crosswalk(self):
        raw = b'https://anonymous.4open.science/r/SemWeaver-FD1E/ anonymous-user'
        self.package.put('notes.md', raw)
        public = (self.root / 'notes.md').read_bytes()
        self.assertNotIn(b'anonymous-user', public)
        self.assertNotIn(b'anonymous-user', public)
        self.assertIn(b'anonymous.4open.science', public)
        self.assertEqual(self.package.crosswalk[0]['original_sha256'], __import__('hashlib').sha256(raw).hexdigest())
        self.assertEqual(self.package.redactions[0]['original_sha256'], self.package.crosswalk[0]['original_sha256'])

    def test_public_byte_tamper_rejected(self):
        self.package.put('one.txt', b'first')
        self.finish()
        (self.root / 'one.txt').write_bytes(b'other')
        with self.assertRaises(AuditError):
            Auditor(self.root).bytes()

    def test_unlisted_file_rejected(self):
        self.package.put('one.txt', b'first')
        self.finish()
        (self.root / 'extra.txt').write_bytes(b'extra')
        with self.assertRaises(AuditError):
            Auditor(self.root).bytes()

    def test_internal_link_rejected(self):
        self.package.put('one.txt', b'first')
        self.finish()
        (self.root / 'alias.txt').symlink_to(self.root / 'one.txt')
        with self.assertRaises(AuditError):
            Auditor(self.root).bytes()

    def test_live_plan_never_certifies_release(self):
        p = Path(self.temp.name) / 'one.json'
        p.write_text('{}')
        plan = describe({Path('project/phase/one.json'): p})
        self.assertEqual(plan['status'], 'provisional_export_inventory_not_a_release')

    def test_corrected_starting_raw_tree_is_hash_bound(self):
        project = Path(self.temp.name)
        data = project / 'data'
        validation = data / 'corrected' / 'RESULT.json'
        validation.parent.mkdir(parents=True)
        validation.write_text('{"healthy": true}')
        digest = __import__('hashlib').sha256(validation.read_bytes()).hexdigest()
        summary = {'starting': {'rows': [
            {'validation_path': 'corrected/RESULT.json', 'validation_sha256': digest},
            {'validation_path': str(validation), 'validation_sha256': digest},
        ]}}
        self.assertEqual(corrected_starting_roots(summary, data, project), [validation.parent])
        validation.write_text('{"healthy": false}')
        with self.assertRaises(AuditError):
            corrected_starting_roots(summary, data, project)

    def test_corrected_starting_path_escape_is_rejected(self):
        outside = Path(self.temp.name) / 'outside.json'
        outside.write_text('{}')
        summary = {'starting': {'rows': [
            {'validation_path': str(outside), 'validation_sha256': 'unused'},
        ]}}
        with self.assertRaises(AuditError):
            corrected_starting_roots(summary, self.root, self.root)

    def test_post_copy_source_drift_is_rejected(self):
        source = Path(self.temp.name) / 'input.txt'
        source.write_text('before')
        files = {Path('project/input.txt'): source}
        self.package.copy(source, 'project/input.txt')
        verify_export_inputs(files, self.package.crosswalk)
        source.write_text('after')
        with self.assertRaises(AuditError):
            verify_export_inputs(files, self.package.crosswalk)

    def test_historical_dirty_source_is_preserved_not_certified_clean(self):
        repo = Path(self.temp.name) / 'synthetic-dirty-source'
        repo.mkdir()
        subprocess.run(['git', 'init', '-q', str(repo)], check=True)
        source = repo / 'module.py'
        source.write_text('# committed\n')
        subprocess.run(['git', '-C', str(repo), 'add', 'module.py'], check=True)
        subprocess.run(['git', '-C', str(repo), '-c', 'user.name=Artifact Test',
                        '-c', 'user.email=test@example.invalid', 'commit', '-qm', 'synthetic fixture'], check=True)
        source.write_text('# working change\n')
        state, diff = source_state(repo, require_clean=False)
        self.assertTrue(state['tracked_dirty'])
        self.assertEqual(state['dirty_paths'], ['module.py'])
        self.assertIn(b'+# working change', diff)
        self.assertEqual(source.read_text(), '# working change\n')
        with self.assertRaises(AuditError):
            source_state(repo, require_clean=True)

    def test_source_archive_merge_identical_and_include_missing_entries(self):
        repo = Path(self.temp.name) / 'synthetic-source'
        repo.mkdir()
        subprocess.run(['git', 'init', '-q', str(repo)], check=True)
        (repo / 'module.py').write_text('# synthetic module\n')
        (repo / 'entry.py').write_text('# synthetic entry\n')
        subprocess.run(['git', '-C', str(repo), 'add', 'module.py', 'entry.py'], check=True)
        subprocess.run(['git', '-C', str(repo), '-c', 'user.name=Artifact Test',
                        '-c', 'user.email=test@example.invalid', 'commit', '-qm', 'synthetic fixture'], check=True)
        self.package.copy(repo / 'module.py', 'project/source/module.py')
        self.package.git_archive(repo, 'project/source', merge_identical=True)
        self.assertEqual((self.root / 'project/source/entry.py').read_text(), '# synthetic entry\n')
        self.assertEqual(len(self.package.crosswalk), 2)

    def test_conflicting_source_archive_never_overwrites_copy(self):
        repo = Path(self.temp.name) / 'synthetic-source'
        repo.mkdir()
        subprocess.run(['git', 'init', '-q', str(repo)], check=True)
        (repo / 'module.py').write_text('# committed\n')
        subprocess.run(['git', '-C', str(repo), 'add', 'module.py'], check=True)
        subprocess.run(['git', '-C', str(repo), '-c', 'user.name=Artifact Test',
                        '-c', 'user.email=test@example.invalid', 'commit', '-qm', 'synthetic fixture'], check=True)
        self.package.put('project/source/module.py', b'# conflicting copy\n')
        with self.assertRaises(AuditError):
            self.package.git_archive(repo, 'project/source', merge_identical=True)
        self.assertEqual((self.root / 'project/source/module.py').read_bytes(), b'# conflicting copy\n')

    @unittest.skipUnless(os.environ.get('SEMWEAVER_REAL_EXPORT_AUDIT') == '1',
                         'Optional read-only integration test on actual private main39 evidence')
    def test_real_main39_redaction_compression_reaggregation(self):
        phase = PROJECT / 'final-native-20260928'
        main = json.loads((phase / 'FINAL39_SUMMARY.json').read_text())
        selected = {phase / 'FINAL39_SUMMARY.json', phase / 'BASELINE_NATURAL_REUSE_AUDIT.json',
                    PROJECT / 'LLM-Native/artifacts/fse_revision/generate_only_knighter_gpt6luna_high_v8/BATCH_RESULT.json'}
        for method, block in {'starting': main['starting'], **main['portfolios']}.items():
            for row in block['rows']:
                selected.update((Path(row['candidate']), Path(row['origin'])))
                if method in ('native', 'no_internal'):
                    cell = Path(row['cell'])
                    selected.update(cell / n for n in ('RUN_PLAN.json', 'CONFIG.json', 'RESULT.json'))
                    result = json.loads((cell / 'RESULT.json').read_text())
                    for attempt in result['rows']:
                        path = Path(attempt['attempt'])
                        selected.update(path / n for n in ('RUN_MANIFEST.json', 'SAGenTestChecker.cpp'))
                        v = path / 'frozen_validation/RESULT.json'
                        if v.exists():
                            selected.add(v)
                elif method == 'knighter':
                    if row['baseline_branch'] == 'actual_zero_response_no_report':
                        selected.add(Path(row['branch_record']))
                    elif row['baseline_branch'] == 'actual_recorded_refinement':
                        cell = Path(row['cell'])
                        selected.update(cell / n for n in ('PAIRED_RESULT.json', 'exchanges.health.json',
                                                          'exchanges.jsonl', 'EXECUTION_MANIFEST.json'))
                        summaries = list((cell / 'output').glob('*/MATCHED_BASELINE_RESULT.json'))
                        self.assertEqual(len(summaries), 1)
                        selected.add(summaries[0])
                        selected.update((summaries[0].parent / 'refinements').glob('attempt_*.cpp'))
                        selected.update((cell / 'paired-replay').glob('attempt-*/RESULT.json'))
        for source in sorted(selected):
            self.package.copy(source, Path('project') / source.relative_to(PROJECT))
        self.finish()
        public = Auditor(self.root)
        public.bytes()
        public.main()
        self.assertEqual(len(public.cells), 78)
        compressed = Path(self.temp.name) / 'compressed'
        expanded = Path(self.temp.name) / 'expanded'
        pack(self.root, compressed)
        unpack(compressed, expanded)
        restored = Auditor(expanded)
        restored.bytes()
        restored.main()
        self.assertEqual(restored.pairs, public.pairs)
        self.assertEqual(len(restored.cells), len(public.cells))

    @unittest.skipUnless(os.environ.get('SEMWEAVER_REAL_EXPORT_AUDIT') == '1',
                         'Optional audit of actual automatic CodeQL and cost records')
    def test_real_codeql_case_and_cost_bindings(self):
        result = Auditor(PROJECT, PROJECT).backend()
        self.assertEqual(result['codeql_subjects'], 1)
        self.assertEqual(result['decodes'], 3)
        self.assertEqual(result['clean_decodes'], 2)
        self.assertEqual(result['warning_recovery_decodes'], 3)
        self.assertEqual(result['cost_side_executions'], 12)
        self.assertFalse(result['developer_hours_measured'])

    @unittest.skipUnless(os.environ.get('SEMWEAVER_REAL_EXPORT_AUDIT') == '1',
                         'Optional audit of actual static and dynamic native evidence')
    def test_real_native_evidence_origin_and_availability(self):
        auditor = Auditor(PROJECT, PROJECT)
        result = auditor.evidence(auditor.main())
        self.assertEqual(result['initial_records'], 193)
        self.assertEqual(result['origin_counts'], {'analyzer_internal': 81, 'analyzer_output': 51, 'source_derived': 61})
        self.assertEqual(result['native_attempts'], 150)
        self.assertEqual(result['usable_dynamic_attempts'], 103)

    @unittest.skipUnless(os.environ.get('SEMWEAVER_REAL_EXPORT_AUDIT') == '1',
                         'Optional reconstruction of actual already received model edits')
    def test_real_received_summary_replay_exact_code(self):
        path = PROJECT / ('final-native-20260928/profile-repairs/e4-focused12/glm-5.3/'
                          'repeat-1/G24_aec8e6bf8391_UAF/offline-no-dispatch')
        auditor = Auditor(PROJECT, PROJECT)
        result = auditor.cell(str(path), 2, 'glm-5.3', 65536)
        self.assertEqual(result['state']['calls'], 2)
        self.assertEqual([result['selected']['vulnerable_alerts'], result['selected']['fixed_alerts']], [3, 3])

    @unittest.skipUnless(os.environ.get('SEMWEAVER_REAL_EXPORT_AUDIT') == '1',
                         'Optional public-byte replay reconstruction audit')
    def test_real_received_replay_after_public_redaction(self):
        cell = PROJECT / ('final-native-20260928/profile-repairs/e4-focused12/glm-5.3/'
                          'repeat-1/G24_aec8e6bf8391_UAF/offline-no-dispatch')
        result = json.loads((cell/'RESULT.json').read_text())
        files = {cell/name for name in ('CONFIG.json', 'RUN_PLAN.json', 'RESULT.json')}
        for row in result['rows']:
            attempt = Path(row['attempt'])
            files.update(attempt/name for name in ('RUN_MANIFEST.json', 'SAGenTestChecker.cpp'))
            if row['paired_valid']: files.add(attempt/'frozen_validation/RESULT.json')
            proof = attempt/'REPLAY_NORMALIZATION.json'
            if proof.exists():
                files.add(proof)
                files.add(attempt/'provider_wire.jsonl')
                certificate = json.loads(proof.read_text())
                files.add(Path(certificate['original_wire']))
        files.update((Path(result['selected']['candidate']), Path(result['selected']['origin'])))
        for source in sorted(files):
            self.package.copy(source, Path('project')/source.relative_to(PROJECT))
        self.finish()
        auditor = Auditor(self.root)
        auditor.bytes()
        public_cell = '/artifact/project/'+cell.relative_to(PROJECT).as_posix()
        verified = auditor.cell(public_cell, 2, 'glm-5.3', 65536)
        self.assertEqual(verified['state']['calls'], 2)
        self.assertEqual(verified['selected']['fixed_alerts'], 3)


if __name__ == '__main__':
    unittest.main()
