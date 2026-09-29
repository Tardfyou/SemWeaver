"""Replay the actual main-study G21 checker on four unchanged retained suites.

No checker editing, no model access, no kernel lane and no substitution into
main-study results. Preserve the old example and every new pass/failure.
"""
import csv
import hashlib
import json
import subprocess
import time
from pathlib import Path

ROOT=Path(__file__).resolve().parent
PROJECT=ROOT.parent
SOURCE=PROJECT/'LLM-Native/SemWeaver-v43'
DATA=PROJECT/'LLM-Native/artifacts/fse_revision'
IMAGE='sha256:2ddb870c297ffbb9b30b901f84da5542a8ae37eb8d6ce332eae1657fdd0014ec'


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()


if __name__=='__main__':
    summary=ROOT/'FINAL39_SUMMARY.json';data=json.loads(summary.read_text())
    row=next(r for r in data['portfolios']['native']['rows'] if r['case_id'].startswith('G21_'))
    checker=Path(row['candidate']);assert sha(checker)==row['candidate_sha256']
    output=ROOT/'motivating-current-main';output.mkdir(exist_ok=False)
    runner=SOURCE/'experiments/robustness/run_csa_metamorphic_suite.py'
    inputs={str(summary):sha(summary),str(checker):sha(checker),str(runner):sha(runner),
            str(Path(__file__)):sha(Path(__file__))}
    suites={}
    for name in ('core','boundary','confirmation','transfer_confirmation'):
        suite=DATA/'motivating_g21_selection_20260927'/name
        rows=list(csv.DictReader((suite/'manifest.csv').open()))
        suites[name]={'path':str(suite),'fixtures':len(rows)}
        inputs[str(suite/'manifest.csv')]=sha(suite/'manifest.csv')
        for fixture in rows:
            file=suite/fixture['source_path'];inputs[str(file)]=sha(file)
    assert sum(s['fixtures'] for s in suites.values())==114
    plan={'method':'unchanged_main_g21_fixture_replay','checker_sha256':row['candidate_sha256'],
          'main_case':row['case_id'],'main_model_responses':row['model_responses'],
          'additional_model_calls':0,'image':IMAGE,'inputs':inputs,'suites':suites,
          'started_at':time.time(),'boundary':'Retained controlled fixtures, not new independent bugs, unseen generalization or a tuning loop. No feedback to a model; no manual checker edit; no change to scored main study.'}
    (output/'RUN_PLAN.json').write_text(json.dumps(plan,indent=2))
    results=[]
    for name,suite in suites.items():
        target=output/name
        command=['docker','run','--rm','--name','semweaver-motivating-main-'+name,
                 '-v',f'{PROJECT}:{PROJECT}','-v',f'{PROJECT/"LLM-Native"}:/work',
                 '-w','/work/SemWeaver-v43','-e','PYTHONPATH=/work/SemWeaver-v43',IMAGE,
                 '/work/SemWeaver/.venv-dev/bin/python',str(runner),
                 '--checker',str(checker),'--suite-dir',suite['path'],'--output-dir',str(target),'--jobs','4']
        with (output/(name+'.stdout')).open('x') as stdout,(output/(name+'.stderr')).open('x') as stderr:
            code=subprocess.run(command,stdout=stdout,stderr=stderr,timeout=1800).returncode
        result_path=target/'RESULT.json'
        assert code==0 and result_path.exists(), 'Execution failure must be repaired, never zero-scored'
        result=json.loads(result_path.read_text());assert result['checker_sha256']==row['candidate_sha256']
        assert all(r['return_code']==0 for r in result['results']), 'Fixture execution failure, unscored'
        results.append({'suite':name,'result':str(result_path),'result_sha256':sha(result_path),
                        **{k:result[k] for k in ('positive_passed','positive_total','negative_passed','negative_total','robust')}})
        print(json.dumps(results[-1]),flush=True)
    assert all(sha(Path(p))==h for p,h in inputs.items()), 'Frozen replay input drift'
    (output/'RESULT.json').write_text(json.dumps({'status':'completed_fixture_replay','checker_sha256':row['candidate_sha256'],
        'additional_model_calls':0,'suites':results,'all114_passed':all(r['robust'] for r in results),
        'boundary':plan['boundary']},indent=2))
    (output/'RUN_MANIFEST.json').write_text(json.dumps({**plan,'status':'completed','finished_at':time.time(),
        'inputs_unchanged':True,'result_sha256':sha(output/'RESULT.json')},indent=2))
