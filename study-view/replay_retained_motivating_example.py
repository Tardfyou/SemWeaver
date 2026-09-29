"""Independently replay the earlier 14-response automatic illustrative checker.

It is not the one-response main-study candidate and cannot replace that row.
Reuse the exact deterministic fixture runner, recording both scheduler inputs.
"""
from pathlib import Path

if __name__=='__main__':
    original=Path(__file__).with_name('replay_current_motivating_example.py')
    source=original.read_text()
    changes={
        "summary=ROOT/'FINAL39_SUMMARY.json'":"summary=PROJECT/'review-revision-20260927/FINAL_SELECTION.json'",
        "row=next(r for r in data['portfolios']['native']['rows'] if r['case_id'].startswith('G21_'))":
        "row={'case_id':data['case_id'],'candidate':data['selected_candidate'],'candidate_sha256':data['candidate_sha256'],'model_responses':data['total_model_calls']}",
        "ROOT/'motivating-current-main'":"ROOT/'motivating-retained-automatic'",
        "'unchanged_main_g21_fixture_replay'":"'retained_automatic_g21_fixture_replay'",
        "'main_case':row['case_id'],'main_model_responses':row['model_responses']":
        "'illustrative_case':row['case_id'],'illustrative_model_responses':row['model_responses']",
        "'semweaver-motivating-main-'":"'semweaver-motivating-retained-'",
        "str(Path(__file__)):sha(Path(__file__))}":
        "str(Path(__file__)):sha(Path(__file__)),str(ROOT/'replay_current_motivating_example.py'):sha(ROOT/'replay_current_motivating_example.py')}",
    }
    for old,new in changes.items():
        assert source.count(old)==1,'Original replay scheduler drift'
        source=source.replace(old,new)
    exec(compile(source,str(original)+'[retained-automatic-checker]', 'exec'),
         {'__name__':'__main__','__file__':str(Path(__file__).resolve())})
