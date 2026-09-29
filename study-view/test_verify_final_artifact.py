"""Failure-injection tests; synthetic files are not empirical study outputs."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from verify_final_artifact import Auditor, AuditError, metrics, same_metrics


class VerificationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.project = Path(self.temp.name)
        self.cell = self.project / 'cell'
        self.attempt = self.cell / 'continuation-01'
        self.attempt.mkdir(parents=True)
        self.cpp = self.attempt / 'SAGenTestChecker.cpp'
        self.cpp.write_text('// synthetic checker\n')
        candidate_sha = hashlib.sha256(self.cpp.read_bytes()).hexdigest()
        self.validation = self.attempt / 'frozen_validation/RESULT.json'
        self.validation.parent.mkdir()
        self.write(self.validation, dict(execution_valid=True, candidate_sha256=candidate_sha,
                                        vulnerable_alerts=1, fixed_alerts=0,
                                        infrastructure_errors=[], scan_failure_artifacts=[]))
        self.manifest = self.attempt / 'RUN_MANIFEST.json'
        self.write(self.manifest, dict(model='glm-5.3', reasoning_effort='high',
                                      model_calls_used=1, max_tokens_per_call=65536,
                                      candidate_sha256=candidate_sha))
        self.row = dict(attempt=str(self.attempt), manifest_sha256=self.sha(self.manifest),
                        paired_valid=True, vulnerable_alerts=1, fixed_alerts=0)
        self.result = dict(status='completed', state={'calls': 1}, rows=[self.row],
                           selected=dict(candidate=str(self.cpp), candidate_sha256=candidate_sha,
                                         origin=str(self.validation), result_sha256=self.sha(self.validation),
                                         vulnerable_alerts=1, fixed_alerts=0))
        self.write(self.cell / 'RESULT.json', self.result)
        self.write(self.cell / 'CONFIG.json', dict(case_id='synthetic',
                    profile={'model': 'glm-5.3', 'reasoning_effort': 'high'}))
        self.write(self.cell / 'RUN_PLAN.json', dict(model_response_cap=2, output_token_ceiling=65536))

    def tearDown(self):
        self.temp.cleanup()

    @staticmethod
    def write(path, obj):
        path.write_text(json.dumps(obj))

    @staticmethod
    def sha(path):
        return hashlib.sha256(path.read_bytes()).hexdigest()

    def audit(self):
        return Auditor(self.project, self.project).cell(str(self.cell), 2, 'glm-5.3', 65536)

    def test_normal_cell(self):
        self.assertEqual(self.audit()['selected']['fixed_alerts'], 0)

    def test_cpp_tamper_rejected(self):
        self.cpp.write_text('// changed\n')
        with self.assertRaises(AuditError):
            self.audit()

    def test_unhealthy_execution_rejected_even_with_updated_hash(self):
        v = json.loads(self.validation.read_text())
        v['execution_valid'] = False
        self.write(self.validation, v)
        self.result['selected']['result_sha256'] = self.sha(self.validation)
        self.write(self.cell / 'RESULT.json', self.result)
        with self.assertRaises(AuditError):
            self.audit()

    def test_invalid_attempt_not_zero(self):
        self.result['rows'][0]['paired_valid'] = False
        self.write(self.cell / 'RESULT.json', self.result)
        with self.assertRaises(AuditError):
            self.audit()

    def test_retained_prior_pair_after_invalid_attempt(self):
        self.result['rows'][0].update(paired_valid=False, vulnerable_alerts=None, fixed_alerts=None)
        self.write(self.cell / 'RESULT.json', self.result)
        self.audit()

    def test_reply_budget_rejected(self):
        m = json.loads(self.manifest.read_text())
        m['model_calls_used'] = 3
        self.write(self.manifest, m)
        self.result['rows'][0]['manifest_sha256'] = self.sha(self.manifest)
        self.result['state']['calls'] = 3
        self.write(self.cell / 'RESULT.json', self.result)
        with self.assertRaises(AuditError):
            self.audit()

    def test_model_drift_rejected(self):
        m = json.loads(self.manifest.read_text())
        m['model'] = 'glm-5.3-flash'
        self.write(self.manifest, m)
        self.result['rows'][0]['manifest_sha256'] = self.sha(self.manifest)
        self.write(self.cell / 'RESULT.json', self.result)
        with self.assertRaises(AuditError):
            self.audit()

    def test_escape_rejected(self):
        with self.assertRaises(AuditError):
            Auditor(self.project, self.project).path('/artifact/project/../../private')

    def test_metric_change_rejected(self):
        rows = [{'vulnerable_alerts': 1, 'fixed_alerts': 0}]
        m = metrics(rows)
        m['F1'] = 0.5
        with self.assertRaises(AuditError):
            same_metrics(m, metrics(rows))

    def test_empty_precision_is_unknown(self):
        self.assertIsNone(metrics([])['precision'])

    def test_partial_main_rejected(self):
        phase = self.project / 'final-native-20260928'
        phase.mkdir()
        self.write(phase / 'FINAL39_SUMMARY.json', {'status': 'partial'})
        with self.assertRaises(AuditError):
            Auditor(self.project, self.project).main()


class RawRecountTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.project = Path(self.temp.name)
        self.data = self.project / 'LLM-Native/artifacts/fse_revision'
        self.scan = self.data / 'synthetic-raw'
        self.scan.mkdir(parents=True)
        self.cpp = self.project / 'checker.cpp'
        self.cpp.write_text('// synthetic checker\n')
        self.origin = self.scan / 'RESULT.json'
        self.raw = dict(case_id='synthetic', execution_valid=True, build_return_code=0,
                        candidate_sha256=VerificationTests.sha(self.cpp), vulnerable_alerts=2,
                        fixed_alerts=0, vulnerable_object_counts={'src/x.o': 2},
                        fixed_object_counts={'src/x.o': 0}, infrastructure_errors=[], scan_failure_artifacts=[])
        VerificationTests.write(self.origin, self.raw)
        for side in ('vulnerable', 'fixed'):
            directory = self.scan / side / 'src-x' / 'scan-1'
            directory.mkdir(parents=True)
        for name in ('report-one.html', 'report-two.html'):
            (self.scan / 'vulnerable/src-x/scan-1' / name).write_text('identical report bytes')
        (self.scan / 'vulnerable/src-x/scan-1/index.html').write_text('not a report')
        (self.scan / 'VALIDATION_RUN.log').write_text('\n'.join(
            f'Running: scan -o /artifact/project/scan/{side}/src-x make src/x.o'
            for side in ('vulnerable', 'fixed')))
        (self.scan / 'build_logs').mkdir()
        for name in ('build_stdout_1.log', 'build_stderr_1.log'):
            (self.scan / 'build_logs' / name).write_text('retained build log')
        corrected = self.data / 'scanfix_v26_corrected_summary_v1'
        corrected.mkdir()
        self.summary = corrected / 'RESULT.json'
        VerificationTests.write(self.summary, {'starting': {'rows': []}})
        self.row = dict(candidate=str(self.cpp), candidate_sha256=VerificationTests.sha(self.cpp),
                        origin=str(self.origin), result_sha256=VerificationTests.sha(self.origin),
                        vulnerable_alerts=2, fixed_alerts=0)

    def tearDown(self):
        self.temp.cleanup()

    def audit(self):
        auditor = Auditor(self.project, self.project)
        auditor.pair(self.row)
        return auditor.raw_csa_counts()

    def test_logical_reports_not_content_deduplicated(self):
        result = self.audit()
        self.assertEqual(result['logical_report_files'], 2)
        self.assertEqual(result['healthy_logged_zero_object_scans'], 1)

    def test_missing_report_rejected(self):
        (self.scan / 'vulnerable/src-x/scan-1/report-two.html').unlink()
        with self.assertRaises(AuditError):
            self.audit()

    def test_zero_requires_scan_log_not_empty_tree(self):
        (self.scan / 'VALIDATION_RUN.log').write_text('Running: scan -o /scan/vulnerable/src-x make src/x.o')
        with self.assertRaises(AuditError):
            self.audit()

    def test_zero_requires_build_logs(self):
        (self.scan / 'build_logs/build_stdout_1.log').unlink()
        with self.assertRaises(AuditError):
            self.audit()

    def test_extra_unattributed_report_rejected(self):
        other = self.scan / 'fixed/other'
        other.mkdir()
        (other / 'report-extra.html').write_text('extra report')
        with self.assertRaises(AuditError):
            self.audit()

    def test_copied_receipt_resolves_hash_bound_corrected_origin(self):
        copied = self.project / 'copied-RESULT.json'
        copied.write_bytes(self.origin.read_bytes())
        VerificationTests.write(self.summary, {'starting': {'rows': [dict(
            case_id='synthetic', validation_path='synthetic-raw/RESULT.json',
            validation_sha256=VerificationTests.sha(self.origin))]}})
        self.row['origin'] = str(copied)
        self.assertEqual(self.audit()['logical_report_files'], 2)

    def test_repeated_origin_not_recounted(self):
        auditor = Auditor(self.project, self.project)
        auditor.pair(self.row)
        auditor.pair(self.row)
        self.assertEqual(auditor.raw_csa_counts()['distinct_validation_origins'], 1)


class BaselineReplayTests(unittest.TestCase):
    write = staticmethod(VerificationTests.write)
    sha = staticmethod(VerificationTests.sha)

    def setUp(self):
        VerificationTests.setUp(self)
        self.output = self.cell / 'output' / 'run'
        self.output.mkdir(parents=True)
        (self.output / 'refinements').mkdir()
        candidate = self.output / 'refinements/attempt_1.cpp'
        candidate.write_bytes(self.cpp.read_bytes())
        self.replay = self.cell / 'paired-replay/attempt-1/RESULT.json'
        self.replay.parent.mkdir(parents=True)
        self.replay.write_bytes(self.validation.read_bytes())
        self.outer = self.output / 'MATCHED_BASELINE_RESULT.json'
        self.write(self.outer, dict(execution_valid=True, model_calls_used=1,
                                   results=[dict(attempt_id=1, refined=True)]))
        self.start = dict(candidate_sha256='separate synthetic starting checker',
                          vulnerable_alerts=0, fixed_alerts=0)
        self.paired = dict(model_responses=1, baseline_summary_sha256=self.sha(self.outer),
                           selected=dict(candidate_sha256=self.sha(candidate), vulnerable_alerts=1, fixed_alerts=0),
                           rows=[dict(attempt_id=1, upstream_refined=True, candidate_sha256=self.sha(candidate),
                                      result_sha256=self.sha(self.replay), vulnerable_alerts=1,
                                      fixed_alerts=0, common_adoption=True)])

    def tearDown(self):
        VerificationTests.tearDown(self)

    def test_baseline_raw_rows_reconstruct_retention(self):
        auditor = Auditor(self.project, self.project)
        auditor.baseline_replay(self.cell, self.paired, self.start)
        self.assertEqual(auditor.baseline_emitted_pairs, 1)

    def test_baseline_adoption_tamper_rejected(self):
        self.paired['rows'][0]['common_adoption'] = False
        with self.assertRaises(AuditError):
            Auditor(self.project, self.project).baseline_replay(self.cell, self.paired, self.start)

    def test_baseline_missing_emitted_validation_rejected(self):
        self.replay.unlink()
        with self.assertRaises(AuditError):
            Auditor(self.project, self.project).baseline_replay(self.cell, self.paired, self.start)


if __name__ == '__main__':
    unittest.main()
