"""Offline protocol fixtures; no model call or experimental result is produced."""
import ast
import json
import tempfile
import unittest
from pathlib import Path
from run_crash_checkpoint_repair import patched_source,runner


def ingest_function():
    tree=ast.parse(patched_source())
    cell=next(n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name=='cell')
    ingest=next(n for n in cell.body if isinstance(n,ast.FunctionDef) and n.name=='ingest')
    for i,node in enumerate(ingest.body):
        if isinstance(node,ast.Nonlocal):ingest.body[i]=ast.Global(names=node.names)
    return compile(ast.fix_missing_locations(ast.Module(body=[ingest],type_ignores=[])),'offline-ingest-fixture','exec')


class CheckpointTest(unittest.TestCase):
    def fixture(self,directory,classification='reproduced_generated_checker_null_callee_assertion',valid=False):
        root=Path(directory);attempt=root/'attempt';attempt.mkdir()
        candidate=attempt/'SAGenTestChecker.cpp';candidate.write_text('OFFLINE_FIXTURE_NOT_SCORED')
        manifest={'candidate_sha256':runner.sha(candidate),'model':'gpt-6-luna','reasoning_effort':'high',
                  'temperature':0,'model_calls_used':1,'success':True}
        (attempt/'RUN_MANIFEST.json').write_text(json.dumps(manifest))
        validation={'candidate_sha256':runner.sha(candidate),'execution_valid':valid,
                    'vulnerable_alerts':1 if valid else 0,'fixed_alerts':0,
                    'infrastructure_errors':[],'scan_failure_artifacts':[] if valid else [{'kind':'offline_assertion_fixture'}]}
        result=root/'fresh-result.json';result.write_text(json.dumps(validation))
        certificate={'attempt':str(attempt),'classification':classification,'rechecked_result':str(result),
                     'rechecked_result_sha256':runner.sha(result)}
        namespace={'Path':Path,'read':runner.read,'sha':runner.sha,'quality':lambda p:set(),
                   'state':{'calls':4,'stagnation':2,'invalid_streak':2},'best':{'vulnerable_alerts':0,'fixed_alerts':0},
                   'required':set(),'latest':None,'rows':[],'arm':'native','CERTIFICATE':certificate,
                   'update_state':runner.update_state,'improves':runner.improves}
        exec(ingest_function(),namespace)
        return attempt,namespace

    def test_crash_response_charged_but_not_scored_or_false_stopping(self):
        with tempfile.TemporaryDirectory() as directory:
            attempt,ns=self.fixture(directory)
            ns['ingest'](attempt)
            self.assertEqual(ns['state']['calls'],5)
            self.assertEqual(ns['state']['invalid_streak'],2)
            self.assertEqual(ns['state']['stagnation'],2)
            self.assertFalse(ns['rows'][0]['paired_valid'])
            self.assertIsNone(ns['rows'][0]['vulnerable_alerts'])
            self.assertTrue(ns['rows'][0]['execution_interruption_unscored'])
            self.assertIsNone(runner.stop_reason(ns['state']))

    def test_other_crash_not_silently_classified(self):
        with tempfile.TemporaryDirectory() as directory:
            attempt,ns=self.fixture(directory,classification='different_unknown_crash')
            with self.assertRaisesRegex(RuntimeError,'Validator infrastructure failure'):
                ns['ingest'](attempt)

    def test_successful_fresh_replay_keeps_normal_retention(self):
        with tempfile.TemporaryDirectory() as directory:
            attempt,ns=self.fixture(directory,classification='fresh_paired_execution_valid',valid=True)
            ns['ingest'](attempt)
            self.assertEqual(ns['state']['calls'],5)
            self.assertEqual(ns['state']['invalid_streak'],0)
            self.assertTrue(ns['rows'][0]['adopted_improvement'])
            self.assertTrue(ns['state']['retained_pds'])
            self.assertEqual(ns['best']['origin'],ns['CERTIFICATE']['rechecked_result'])


if __name__=='__main__':unittest.main()
