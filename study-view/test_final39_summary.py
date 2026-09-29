"""Offline final-table safety tests; fixtures are not experimental subjects."""
import json
import tempfile
import unittest
from pathlib import Path
from build_final39_summary import bound,metrics,sha


class FinalSummaryTests(unittest.TestCase):
    def test_version_metrics_not_raw_warning_precision(self):
        rows=[{'vulnerable_alerts':100,'fixed_alerts':0},{'vulnerable_alerts':0,'fixed_alerts':73}]
        values=metrics(rows)
        self.assertEqual((values['TP'],values['FP'],values['FN'],values['TN']),(1,1,1,1))
        self.assertEqual(values['F1'],0.5)
        self.assertEqual(values['fixed_warning_reports'],73)

    def test_empty_metrics_do_not_divide_by_zero(self):
        self.assertEqual(metrics([])['F1'],0)

    def fixture(self,directory):
        root=Path(directory);candidate=root/'fixture.cpp';candidate.write_text('OFFLINE_NOT_SCORED')
        result=root/'fixture-result.json'
        data={'execution_valid':True,'candidate_sha256':sha(candidate),'vulnerable_alerts':1,'fixed_alerts':0,
              'infrastructure_errors':[],'scan_failure_artifacts':[]}
        result.write_text(json.dumps(data))
        row={'candidate':str(candidate),'candidate_sha256':sha(candidate),'origin':str(result),
             'result_sha256':sha(result),'vulnerable_alerts':1,'fixed_alerts':0}
        return row,data,result

    def test_normal_pair_accepted(self):
        with tempfile.TemporaryDirectory() as directory:
            row,_,_=self.fixture(directory);self.assertEqual(bound(row),row)

    def test_invalid_execution_never_zero_warning_success(self):
        with tempfile.TemporaryDirectory() as directory:
            row,data,path=self.fixture(directory);data['execution_valid']=False
            path.write_text(json.dumps(data));row['result_sha256']=sha(path)
            with self.assertRaises(AssertionError):bound(row)

    def test_infrastructure_artifact_never_scored(self):
        with tempfile.TemporaryDirectory() as directory:
            row,data,path=self.fixture(directory);data['scan_failure_artifacts']=[{'kind':'offline_crash'}]
            path.write_text(json.dumps(data));row['result_sha256']=sha(path)
            with self.assertRaises(AssertionError):bound(row)

    def test_checker_identity_drift_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            row,_,_=self.fixture(directory);Path(row['candidate']).write_text('DIFFERENT_OFFLINE_FIXTURE')
            with self.assertRaises(AssertionError):bound(row)


if __name__=='__main__':unittest.main()
