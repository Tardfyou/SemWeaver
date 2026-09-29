"""Synthetic audit tests; these fixtures are not scientific observations."""
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
import audit_remaining_matrix as audit


class AuditTests(unittest.TestCase):
    def test_missing_focus_never_selects_old_glm(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(audit,'ROOT',Path(directory)):
            profile,folder,errors=audit.select_profile(
                {'model':'glm-5.3','reasoning_effort':''}, {'repeated_samples':['fixture']}, True)
            self.assertEqual(profile['reasoning_effort'],'high')
            self.assertEqual(folder,Path(directory)/'e4-focused12/glm-5.3')
            self.assertIn('focused_configuration_missing',errors)

    def test_pending_alerts_are_only_checkpoints(self):
        basic={'integrity_errors':[],'verified_complete':False,'status':'running',
               'model_responses':0,'retained_alerts':[0,0]}
        with tempfile.TemporaryDirectory() as directory, patch.object(audit,'ROOT',Path(directory)), \
             patch.object(audit.watch_final.integrity,'inspect_cell',return_value=basic):
            row=audit.inspect(Path(directory)/'fixture','fixture','native',
                              {'model':'glm-5.3','reasoning_effort':'high'},2,65536)
            self.assertEqual(row['retained_alerts'],[None,None])
            self.assertEqual(row['checkpoint_alerts'],[0,0])
            self.assertFalse(row['verified_complete'])


if __name__=='__main__':
    unittest.main()
