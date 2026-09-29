import unittest
from run_profile_cell import stop_reason


class ProfilePolicyTests(unittest.TestCase):
    def state(self, **values):
        result = {'calls': 0, 'stagnation': 0, 'invalid_streak': 0}
        result.update(values)
        return result

    def test_two_response_robustness_ceiling(self):
        self.assertEqual(stop_reason(self.state(calls=2), 2), 'model_call_ceiling')

    def test_main_protocol_not_clipped_to_two(self):
        self.assertIsNone(stop_reason(self.state(calls=2), 32))

    def test_stagnation_stop_preserved(self):
        self.assertEqual(stop_reason(self.state(stagnation=3), 32), 'three_validations_without_retained_improvement')

    def test_ineffective_attempt_stop_preserved(self):
        self.assertEqual(stop_reason(self.state(invalid_streak=3), 32), 'three_model_attempts_without_valid_candidate')

    def test_clean_signal_saturation(self):
        self.assertEqual(stop_reason(self.state(retained_pds=True), 32), 'retained_positive_zero_fixed_saturation')

    def test_rejected_raw_saturation_not_success(self):
        self.assertEqual(stop_reason(self.state(guard_rejected_pds=True), 32), 'raw_saturation_rejected_requires_quality_diagnosis')


if __name__ == '__main__':
    unittest.main()
