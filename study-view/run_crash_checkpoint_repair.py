"""Diagnose a consumed candidate crash, then ask the model to repair it.

Only a fresh, hash-bound reproduction of the exact null-dyn_cast assertion
permits this bridge. No checker is edited, no crashed scan is an alert count,
and the consumed response remains charged to the original32-response budget.
"""
import json
import sys
from pathlib import Path
import run_observer_repaired_cell as observer

coverage=observer.coverage
runner=coverage.runner
ROOT=Path(__file__).resolve().parent


def diagnose(root, cfg):
    attempt=Path(cfg['crashed_attempt'])
    candidate=attempt/'SAGenTestChecker.cpp'
    original=runner.read(attempt/'RUN_MANIFEST.json')
    assert original['model_calls_used']==1 and original['success']
    assert runner.sha(candidate)==original['candidate_sha256']
    output=root/'rechecked-candidate'
    command=[sys.executable,str(runner.SOURCE/'experiments/knighter/experiment/validate_frozen_csa_candidate.py'),
             '--candidate',str(candidate),'--case-dir',str(runner.DATA/'generate_only_materialized_v1/cases'/cfg['case_id']),
             '--linux-dir',cfg['linux_dir'],'--output-dir',str(output),
             '--backend-workspace',str(root/'recheck-backend'),'--jobs','4']
    rc=observer.invoke(command,root/'recheck-validation',timeout=1800)
    validation=runner.read(output/'RESULT.json')
    assert validation['candidate_sha256']==runner.sha(candidate)
    assert not validation.get('infrastructure_errors'), 'Host/tool failure remains unscored'
    certificate={'attempt':str(attempt),'candidate_sha256':runner.sha(candidate),
                 'rechecked_result':str(output/'RESULT.json'),'rechecked_result_sha256':runner.sha(output/'RESULT.json'),
                 'original_result_sha256':runner.sha(attempt/'frozen_validation/RESULT.json'),
                 'validator_return_code':rc,'consumed_responses':1,'scored_checker_modified':False}
    if validation['execution_valid'] and not validation.get('scan_failure_artifacts'):
        certificate['classification']='fresh_paired_execution_valid'
    else:
        assert validation['build_return_code']==0 and validation.get('scan_failure_artifacts')
        texts=[]
        for artifact in validation['scan_failure_artifacts']:
            path=output/artifact['side']/artifact['path']
            assert runner.sha(path)==artifact['sha256']
            if path.name.endswith('stderr.txt'):
                text=path.read_text(errors='replace')
                if 'dyn_cast on a non-existent value' in text and 'SAGenTestChecker::isPublishCall' in text:
                    assert str(root/'recheck-backend/build/lib/SAGenTestPlugin.so') in text
                    lines=[line for line in text.splitlines() if 'dyn_cast on a non-existent value' in line
                           or 'SAGenTestChecker::isPublishCall' in line or 'SAGenTestChecker::checkPreCall' in line]
                    texts += lines
        assert texts, 'Different crash must be diagnosed, never relabelled'
        assert 'dyn_cast<FunctionDecl>(Call.getDecl())' in candidate.read_text()
        certificate['classification']='reproduced_generated_checker_null_callee_assertion'
        certificate['diagnostics']='\n'.join(texts)
        feedback=root/'execution-feedback';feedback.mkdir()
        derived={**original,'success':False,'latest_failure_title':'paired_scan_candidate_assertion',
                 'latest_failure_text':'The hash-bound candidate compiled but crashed in a fresh paired replay. '
                    'These are execution diagnostics, not native analyzer evidence and not zero-alert results. '
                    'Repair the null-callee assertion while preserving the original callback and detection mechanism.\n'+certificate['diagnostics'],
                 'automatic_execution_feedback_sha256':certificate['rechecked_result_sha256']}
        runner.save(feedback/'RUN_MANIFEST.json',derived)
        (feedback/'run_events.jsonl').open('x').close()
        certificate['feedback_manifest']=str(feedback/'RUN_MANIFEST.json')
        certificate['feedback_manifest_sha256']=runner.sha(feedback/'RUN_MANIFEST.json')
    runner.save(root/'CRASH_RECHECK.json',certificate)
    return certificate


def patched_source():
    # Keep the selected algorithm intact except the interrupted-execution ingest.
    source=Path(runner.__file__).read_text()
    source=source.replace("vpath=attempt/'frozen_validation/RESULT.json'", "vpath=Path(CERTIFICATE['rechecked_result']) if str(attempt)==CERTIFICATE['attempt'] else attempt/'frozen_validation/RESULT.json'")
    old="""        if validation:
            assert validation['candidate_sha256']==sha(candidate)
            if validation.get('infrastructure_errors') or validation.get('scan_failure_artifacts'):
                raise RuntimeError('Validator infrastructure failure, unscored')
        if t['success'] and validation is None:raise RuntimeError('Missing validator, unscored')"""
    new="""        interrupted_execution=False
        if validation:
            assert validation['candidate_sha256']==sha(candidate)
            if validation.get('infrastructure_errors') or validation.get('scan_failure_artifacts'):
                if (str(attempt)==CERTIFICATE['attempt'] and CERTIFICATE['classification']=='reproduced_generated_checker_null_callee_assertion'
                        and sha(vpath)==CERTIFICATE['rechecked_result_sha256']):
                    validation=None;interrupted_execution=True
                else:raise RuntimeError('Validator infrastructure failure, unscored')
        if t['success'] and validation is None and not interrupted_execution:raise RuntimeError('Missing validator, unscored')"""
    assert source.count(old)==1
    source=source.replace(old,new)
    source=source.replace('        update_state(state,valid,accepted)', '        if not interrupted_execution:update_state(state,valid,accepted)')
    source=source.replace("'lost_controlled_hits':lost,'state_after':dict(state)","'lost_controlled_hits':lost,'execution_interruption_unscored':interrupted_execution,'state_after':dict(state)")
    # The exact fresh replay is used as feedback if it succeeded; otherwise the
    # derived failed-run receipt carries the real assertion to the model.
    source=source.replace("feedback=latest/'frozen_validation/RESULT.json'", "feedback=Path(CERTIFICATE['rechecked_result']) if str(latest)==CERTIFICATE['attempt'] else latest/'frozen_validation/RESULT.json'")
    return source


def main(config_path):
    cfg=runner.read(config_path);root=config_path.parent
    certificate=diagnose(root,cfg)
    original_invoke=runner.invoke
    def invoke(command,output,env=None,timeout=None):
        if (len(command)>1 and Path(command[1]).name=='run_semweaver_treatment_case.py'
                and '--starting-candidate' in command
                and runner.sha(Path(command[command.index('--starting-candidate')+1]))==certificate['candidate_sha256']
                and certificate['classification']=='reproduced_generated_checker_null_callee_assertion'):
            command=list(command)
            index=command.index('--feedback-result')
            command[index:index+2]=['--feedback-run-manifest',certificate['feedback_manifest']]
            env=dict(env);env['SEMWEEVER_CHECKER_EXECUTION_TRACE']='0'
            runner.save(output.with_name(output.name+'-CRASH_FEEDBACK.json'),{
                'reason':'Current candidate cannot execute, so no trace is fabricated',
                'certificate_sha256':runner.sha(root/'CRASH_RECHECK.json'),
                'feedback_manifest_sha256':certificate['feedback_manifest_sha256'],
                'model_calls_before_repair':cfg['prefix_calls']})
        return original_invoke(command,output,env,timeout)
    runner.invoke=invoke
    source=patched_source()
    # A reproduced crash is routed by invoke; successful fresh replay is normal.
    namespace={**runner.__dict__,'__name__':'checkpoint_repaired_runner','CERTIFICATE':certificate,'invoke':invoke}
    exec(compile(source,runner.__file__+'[verified-crash-checkpoint]','exec'),namespace)
    # Executing the frozen module redefines invoke; install the bridge afterward.
    namespace['invoke']=invoke
    namespace['cell'](config_path)


if __name__=='__main__':
    main(Path(sys.argv[1]))
