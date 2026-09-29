"""Offline capacity/prefix safety tests; never count these as experiments."""
import importlib
import subprocess
import unittest
from pathlib import Path
from adaptive_trace_probe import expanded_header

ROOT=Path(__file__).resolve().parent
SOURCE=ROOT.parent/'LLM-Native/SemWeaver-v43'


class CapacityTests(unittest.TestCase):
    def test_only_observer_capacity_changes(self):
        header=SOURCE/'src/research/checker_trace_support.h'
        original=header.read_text();text=expanded_header(header,64000)
        self.assertEqual(text.replace('constexpr unsigned cap = 64000;','constexpr unsigned cap = 16000;'),original)
        self.assertEqual(header.read_text(),original)

    def test_capacity_is_bounded(self):
        with self.assertRaises(AssertionError):expanded_header(SOURCE/'src/research/checker_trace_support.h',100000000)

    def test_real_prefix_verified_without_new_model_call(self):
        previous=subprocess.run
        try:module=importlib.import_module('run_profile_cap_repair')
        finally:subprocess.run=previous
        parent=ROOT/'e4-common64/glm-5.3-flash/repeat-1/G03_1f886a7bfb3f_Null_Pointer_Dereference'
        cfg=module.profile.read(parent/'CONFIG.json');progress=module.profile.read(parent/'PROGRESS.json')
        cfg.update(prefix_parent=str(parent),prefix_calls=1,prefix_attempts=[r['attempt'] for r in progress['rows']])
        state,best,rows,latest,required=module.restore_prefix(cfg,lambda candidate:set(),2)
        self.assertEqual(state['calls'],1);self.assertEqual(len(rows),1)
        self.assertEqual(latest,Path(rows[-1]['attempt']))
        self.assertEqual((best['vulnerable_alerts'],best['fixed_alerts']),(2,2))
        self.assertIsNone(module.profile.stop_reason(state,2))
        cfg['prefix_calls']=0
        with self.assertRaises(AssertionError):module.restore_prefix(cfg,lambda candidate:set(),2)

    def test_prefix_model_mismatch_rejected(self):
        previous=subprocess.run
        try:module=importlib.import_module('run_profile_cap_repair')
        finally:subprocess.run=previous
        parent=ROOT/'e4-common64/glm-5.3-flash/repeat-1/G03_1f886a7bfb3f_Null_Pointer_Dereference'
        cfg=module.profile.read(parent/'CONFIG.json');progress=module.profile.read(parent/'PROGRESS.json')
        cfg.update(prefix_parent=str(parent),prefix_calls=1,prefix_attempts=[r['attempt'] for r in progress['rows']])
        cfg['profile']={**cfg['profile'],'model':'different_model'}
        with self.assertRaisesRegex(AssertionError,'Prefix protocol/config mismatch'):
            module.restore_prefix(cfg,lambda candidate:set(),2)


if __name__=='__main__':unittest.main()
