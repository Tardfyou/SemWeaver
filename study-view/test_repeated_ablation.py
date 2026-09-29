"""Synthetic arithmetic cases only, not experiment observations."""
import unittest
from summarize_repeated_ablation import direction,summarize,ROOT


class DirectionTests(unittest.TestCase):
    def test_noisy_recovery_kept(self):
        self.assertEqual(direction([1,5],[0,0]),'positive_warning_recovery')

    def test_fixed_reduction_needs_retained_hit(self):
        self.assertEqual(direction([1,2],[3,27]),'positive_fixed_warning_reduction')
        self.assertEqual(direction([0,0],[3,27]),'negative_lost_version_hit')

    def test_more_fixed_noise_is_negative(self):
        self.assertEqual(direction([2,2],[2,0]),'negative_more_fixed_warnings')

    def test_pending_is_not_tie(self):
        self.assertIsNone(direction(None,[0,0]))
        self.assertEqual(direction([0,0],[0,0]),'tie_or_other')

    def test_missing_decode_not_assumed_consistent(self):
        subjects=[f'fixture-{i}' for i in range(12)]
        first=[];extra=[]
        for case in subjects:
            cells={arm:{'verified_complete':True,'integrity_errors':[],
                        'retained_alerts':[0,0],'model_responses':1,'path':case}
                   for arm in ('native','no_internal')}
            first.append({'sample_id':case,**cells})
            for repeat in (2,3):
                for arm in ('native','no_internal'):
                    extra.append({'case_id':case,'arm':arm,**cells[arm],
                                  'parent':str(ROOT/'e3-selected-repeats'/f'repeat-{repeat}'/arm/case)})
        extra[2]['verified_complete']=False
        result=summarize(subjects,{'rows':first},{'e3':{'rows':extra}})
        self.assertEqual(result['complete_paired_decodes'],35)
        self.assertEqual(result['complete_three_decode_subjects'],11)
        self.assertIsNone(result['rows'][0]['direction_consistency'])
        self.assertIsNone(result['rows'][0]['repeats'][2]['outcome'])


if __name__=='__main__':unittest.main()
