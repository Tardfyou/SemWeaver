"""Actually execute all24 upstream no-report branches without model calls."""
import hashlib
import json
import subprocess
import time
from pathlib import Path

ROOT=Path(__file__).resolve().parent
PROJECT=ROOT.parent
SOURCE=PROJECT/'LLM-Native/SemWeaver-v43'
DATA=PROJECT/'LLM-Native/artifacts/fse_revision'
LANE=PROJECT/'LLM-Native/SemWeaver/artifacts/external/linux-g01-timeout-audit-clean'
def read(path):return json.loads(Path(path).read_text())
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def save(path,value):
    with path.open('x') as h:json.dump(value,h,indent=2)

if __name__=='__main__':
    starting=read(DATA/'scanfix_v26_corrected_summary_v1/RESULT.json')['starting']['rows']
    cases=[r['case_id'] for r in starting if r['fixed_alerts']==0]
    assert len(cases)==24
    output=ROOT/'knighter-no-report';output.mkdir(exist_ok=False)
    source_inputs={str(Path(__file__)):sha(Path(__file__)),str(ROOT/'run_recorded_baseline.py'):sha(ROOT/'run_recorded_baseline.py'),
                   str(PROJECT/'native-state-20260928/record_knighter_exchanges.py'):sha(PROJECT/'native-state-20260928/record_knighter_exchanges.py')}
    for folder in ('experiments/knighter/baseline','experiments/knighter/experiment','experiments/robustness/case07_negative_control'):
        for p in (SOURCE/folder).rglob('*'):
            if p.is_file() and '__pycache__' not in p.parts and p.suffix!='.pyc':source_inputs[str(p)]=sha(p)
    save(output/'PLAN.json',{'subjects':cases,'input_hashes':source_inputs,'model':'gpt-6-luna','reasoning_effort':'high',
         'response_ceiling':32,'max_tokens':16384,'outer_max_tries':2,'max_fp_reports':5,
         'boundary':'Real upstream no-report branch, not an inferred score; independent paired starting validation supplies the unchanged outcome.'})
    rows=[]
    for case in cases:
        cell=output/case;cell.mkdir()
        case_dir=DATA/'generate_only_materialized_v1/cases'/case
        reports=PROJECT/'native-trace-20260928/fixed_reports'/case/'fixed'
        assert not list(reports.glob('report-*.html'))
        start_path=ROOT/'arm64-starting/RESULT.json' if case.startswith('G04_') else PROJECT/'native-trace-20260928/starting'/case/'RESULT.json'
        start=read(start_path);checker=case_dir/'csa/SAGenTestChecker.cpp'
        assert start['execution_valid'] and start['candidate_sha256']==sha(checker) and start['fixed_alerts']==0
        image='sha256:2ddb870c297ffbb9b30b901f84da5542a8ae37eb8d6ce332eae1657fdd0014ec'
        command=['docker','run','--rm','--name','semweaver-baseline-no-report-'+case[:3],
             '-v',f'{PROJECT}:{PROJECT}','-v',f'{PROJECT/"LLM-Native"}:/work',
             '-v','/anonymous/home/.census-key:/run/private/gpt-key:ro','-w','/work/SemWeaver-v43',
             '-e','PYTHONPATH=/work/SemWeaver-v43','-e','PATH=/usr/lib/llvm-18/bin:/usr/bin:/bin',
             '-e','GIT_CONFIG_COUNT=2','-e','GIT_CONFIG_KEY_0=safe.directory','-e',f'GIT_CONFIG_VALUE_0={LANE}',
             '-e','GIT_CONFIG_KEY_1=safe.directory','-e',f'GIT_CONFIG_VALUE_1={SOURCE}',image,
             '/work/SemWeaver/.venv-dev/bin/python',str(ROOT/'run_recorded_baseline.py'),
             '--source-root',str(SOURCE),'--exchange-log',str(cell/'exchanges.jsonl'),'--',
             '--case-dir',str(case_dir),'--fixed-report-dir',str(reports),'--linux-dir',str(LANE),
             '--output-root',str(cell/'output'),'--backend-workspace',str(cell/'backend'),
             '--model','gpt-6-luna','--reasoning-effort','high','--temperature','0','--max-tokens','16384',
             '--max-model-calls','32','--max-tries','2','--max-fp-reports','5','--jobs','4']
        with (cell/'container.stdout').open('x') as stdout,(cell/'container.stderr').open('x') as stderr:
            code=subprocess.run(command,stdout=stdout,stderr=stderr,timeout=600).returncode
        assert code==0
        health=read(cell/'exchanges.health.json')
        assert health['status']=='completed' and health['consumed_model_responses']==0 and not health['errors']
        assert (cell/'exchanges.jsonl').read_bytes()==b''
        summary_path=list((cell/'output').glob('*/MATCHED_BASELINE_RESULT.json'));assert len(summary_path)==1
        summary=read(summary_path[0])
        assert summary['completed'] and summary['execution_valid'] and summary['no_fixed_reports'] and summary['model_calls_used']==0
        assert summary['checker_sha256']==sha(checker) and all(not r['refined'] for r in summary['results'])
        assert all(sha(p)==h for p,h in source_inputs.items())
        row={'case_id':case,'status':'completed_no_report_no_op','model_responses':0,
             'candidate':str(checker),'candidate_sha256':sha(checker),'vulnerable_alerts':start['vulnerable_alerts'],
             'fixed_alerts':start['fixed_alerts'],'origin':str(start_path),'result_sha256':sha(start_path),
             'summary_path':str(summary_path[0]),'summary_sha256':sha(summary_path[0]),
             'health_sha256':sha(cell/'exchanges.health.json'),'return_code':code}
        save(cell/'NO_REPORT_RESULT.json',row);rows.append(row)
        print(json.dumps({'actual_no_report_case':case,'model_responses':0}),flush=True)
    save(output/'RESULT.json',{'status':'completed','rows':rows,'model_responses':0,
         'plan_sha256':sha(output/'PLAN.json'),'finished_at':time.time()})
