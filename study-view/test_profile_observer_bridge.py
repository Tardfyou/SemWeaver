"""Offline command-routing tests; no SDK request or checker execution."""
import importlib
import subprocess
import unittest
from pathlib import Path


class ProfileBridgeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        previous=subprocess.run
        try:cls.bridge=importlib.import_module('run_profile_observer_repair')
        finally:subprocess.run=previous

    def capture(self,command,env=None,wire=False):
        captured=[]
        previous=self.bridge.original
        self.bridge.original=lambda cmd,*args,**kwargs:captured.append((cmd,kwargs)) or 0
        try:
            function=self.bridge.wire.recorded_run if wire else self.bridge.run
            self.assertEqual(function(command,env=env),0)
            return captured[0]
        finally:self.bridge.original=previous

    def test_direct_treatment_preserves_all_arguments(self):
        arguments=['--output-dir','/offline/cell','--max-tokens','65536','--max-model-calls','1','--evidence-mode','native']
        cmd,_=self.capture(['python','/offline/run_semweaver_treatment_case.py',*arguments])
        self.assertEqual(Path(cmd[1]).name,'repaired_pipeline_entry.py')
        self.assertEqual(cmd[cmd.index('--target-script')+1],'/offline/run_semweaver_treatment_case.py')
        self.assertEqual(cmd[cmd.index('--')+1:],arguments)

    def test_glm_wire_recording_retained_before_observer_bridge(self):
        arguments=['--output-dir','/offline/cell','--max-tokens','65536','--max-model-calls','1']
        cmd,kwargs=self.capture(['python','/offline/run_semweaver_treatment_case.py',*arguments],
                                 env={'SEMWEEVER_LLM_PROVIDER':'anthropic'},wire=True)
        self.assertEqual(Path(cmd[1]).name,'repaired_pipeline_entry.py')
        self.assertEqual(Path(cmd[cmd.index('--target-script')+1]).name,'provider_wire_entry.py')
        self.assertEqual(kwargs['env']['SEMWEEVER_PROVIDER_WIRE_LOG'],'/offline/cell/provider_wire.jsonl')
        self.assertEqual(cmd[-len(arguments):],arguments)

    def test_validator_not_intercepted(self):
        original=['python','/offline/validate_frozen_csa_candidate.py','--jobs','4']
        cmd,_=self.capture(original)
        self.assertEqual(cmd,original)


if __name__=='__main__':unittest.main()
