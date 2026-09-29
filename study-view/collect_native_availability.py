"""Audit dynamic evidence availability on unique completed main-study attempts.

This is a diagnostic ledger, not target recall or a count of independent bugs.
Static bundle records have a separate denominator. No active inputs are edited.
"""
from collections import Counter
import hashlib
import json
from pathlib import Path
import watch_final

ROOT=Path(__file__).resolve().parent


def read(path):
    return json.loads(Path(path).read_text())


def sha(path):
    digest=hashlib.sha256()
    with Path(path).open('rb') as handle:
        for chunk in iter(lambda:handle.read(1024*1024),b''):digest.update(chunk)
    return digest.hexdigest()


def audit_attempt(attempt):
    manifest=read(attempt/'RUN_MANIFEST.json')
    status=manifest.get('checker_execution_probe_status','missing')
    errors=[];bindings={};available=False;probe=attempt/'checker_execution_probe'

    def bind(path,expected=None):
        try:
            actual=sha(path);bindings[str(path)]=actual
            if expected is not None and actual!=expected:errors.append('hash_mismatch:'+str(path))
        except OSError:errors.append('missing_artifact:'+str(path))

    bind(attempt/'RUN_MANIFEST.json')
    if status=='usable_hash_bound_trace':
        result=read(probe/'RESULT.json');receipt=read(probe/'RUN_MANIFEST.json')
        payload=read(attempt/'checker_execution_feedback.json')
        bind(probe/'RESULT.json');bind(probe/'RUN_MANIFEST.json')
        bind(attempt/'checker_execution_feedback.json')
        scans=result.get('scans',[])
        if (result.get('execution_health')!='completed' or not result.get('diagnostic_parity')
                or not result.get('trace_usable') or not receipt.get('inputs_unchanged')
                or receipt.get('execution_health')!='completed' or not receipt.get('diagnostic_parity')
                or receipt.get('scans')!=scans or len(scans)!=4
                or {(s.get('side'),s.get('mode')) for s in scans}
                   !={(s,m) for s in ('vulnerable','fixed') for m in ('original','traced')}
                or any(not s.get('execution_valid') or s.get('trace_capped') for s in scans)):
            errors.append('invalid_probe_health_or_parity')
        if payload.get('checker_sha256')!=manifest.get('starting_checker_sha256'):
            errors.append('feedback_starting_checker_mismatch')
        for path,digest in receipt.get('inputs',{}).items():bind(Path(path),digest)
        sides=payload.get('sides',[])
        if {s.get('side') for s in sides}!={'vulnerable','fixed'} or len(sides)!=2:
            errors.append('feedback_sides_mismatch')
        for side in sides:
            raw=probe/side['side']/'traced/trace.jsonl'
            if not side.get('raw_sha256'):errors.append('missing_raw_hash')
            bind(raw,side.get('raw_sha256'))
            scan=next((s for s in scans if s.get('side')==side['side'] and s.get('mode')=='traced'),{})
            if not scan.get('trace_events'):errors.append('empty_dynamic_trace')
            if scan.get('trace_sha256')!=side.get('raw_sha256'):errors.append('trace_binding_mismatch')
        available=not errors
    elif status=='not_requested':
        fallback=attempt/'CONTEXTLESS_FALLBACK.json'
        crash=attempt.with_name(attempt.name+'-CRASH_FEEDBACK.json')
        optional=attempt.with_name(attempt.name.removesuffix('-static')+'-FALLBACK.json')
        if fallback.exists():
            status='verified_unavailable_no_supported_context'
            data=read(fallback);proof=attempt/'contextless-parity-probe'
            bind(fallback);bind(proof/'RUN_MANIFEST.json',data['parity_probe_manifest_sha256'])
            bind(proof/'CONTEXTLESS_COVERAGE.json',data['availability_sha256'])
            coverage=read(proof/'CONTEXTLESS_COVERAGE.json')
            if coverage.get('status')!='verified_unavailable' or not coverage.get('diagnostic_copy_is_unmodified'):
                errors.append('unsupported_context_parity_missing')
        elif crash.exists():
            status='unavailable_reproduced_candidate_crash'
            bind(crash);bind(attempt.parent/'CRASH_RECHECK.json',read(crash)['certificate_sha256'])
        elif attempt.name.endswith('-static') and optional.exists():
            data=read(optional);bind(optional);original_probe=Path(data['probe'])
            result=read(original_probe/'RESULT.json');receipt=read(original_probe/'RUN_MANIFEST.json')
            bind(original_probe/'RESULT.json');bind(original_probe/'RUN_MANIFEST.json')
            if (data.get('reason')!='unusable_optional_trace_coverage'
                    or data.get('model_calls_before_fallback')!=0 or not data.get('not_dynamic_evidence')
                    or result.get('execution_health')!='completed' or not result.get('diagnostic_parity')
                    or result.get('trace_usable') or not receipt.get('inputs_unchanged')
                    or receipt.get('scans')!=result.get('scans') or len(result.get('scans',[]))!=4
                    or any(not s.get('execution_valid') for s in result.get('scans',[]))):
                errors.append('invalid_optional_trace_fallback')
            status=('unavailable_capped_optional_trace' if any(s.get('trace_capped') for s in result.get('scans',[]))
                    else 'unavailable_unusable_optional_trace')
            starting=manifest.get('starting_checker_sha256')
            if starting not in receipt.get('inputs',{}).values():errors.append('optional_probe_starting_checker_mismatch')
            for path,digest in receipt.get('inputs',{}).items():bind(Path(path),digest)
        else:errors.append('unexplained_not_requested')
    elif status in ('unavailable_current_candidate_compile_error','unavailable_zero_callbacks_in_validated_scope'):
        unavailable=attempt/'checker_execution_unavailable.json'
        data=read(unavailable);bind(unavailable)
        bind(probe/'RUN_MANIFEST.json',data['probe_manifest_sha256'])
        if data.get('candidate_sha256')!=manifest.get('starting_checker_sha256'):
            errors.append('unavailable_starting_checker_mismatch')
    else:errors.append('unknown_availability_status')
    return {'attempt':str(attempt),'status':status,'dynamic_evidence_available':available,
            'model_responses':manifest.get('model_calls_used'),
            'static_internal_evidence_preloaded':manifest.get('internal_evidence_preloaded'),
            'integrity_errors':errors,'bindings':bindings}


def collect(snapshot):
    rows=[];seen=set()
    for pair in snapshot['rows']:
        if not pair['complete_pair']:continue
        cell=Path(pair['native']['path']);result=read(cell/'RESULT.json')
        for row in result['rows']:
            attempt=Path(row['attempt']).resolve()
            if attempt in seen:continue
            seen.add(attempt)
            try:entry=audit_attempt(attempt)
            except (OSError,KeyError,ValueError,StopIteration) as error:
                entry={'attempt':str(attempt),'status':'audit_error','dynamic_evidence_available':False,
                       'integrity_errors':[type(error).__name__+':'+str(error)]}
            rows.append({'case_id':pair['sample_id'],**entry})
    counts=Counter(row['status'] for row in rows)
    return {'status':'complete' if snapshot['verified_pairs']==39 else 'partial_completed_pairs_only',
            'completed_pairs':snapshot['verified_pairs'],'planned_pairs':39,
            'unique_native_attempts':len(rows),'status_counts':dict(counts),
            'verified_dynamic_available_attempts':sum(r['dynamic_evidence_available'] for r in rows),
            'integrity_error_attempts':sum(bool(r['integrity_errors']) for r in rows),'rows':rows,
            'boundary':'Selected completed native chains only; charged prefixes deduplicated by actual attempt. Availability is per attempt, not independent bugs, source-window fallback records, exhaustive coverage or target recall. Static preloaded evidence is separate from dynamic observations. Partial main pairs and unrelated historical runs are not pooled.'}


if __name__=='__main__':
    output=collect(watch_final.snapshot())
    temporary=ROOT/'NATIVE_EVIDENCE_AVAILABILITY_STATUS.tmp'
    temporary.write_text(json.dumps(output,indent=2))
    temporary.replace(ROOT/'NATIVE_EVIDENCE_AVAILABILITY_STATUS.json')
    print(json.dumps({k:v for k,v in output.items() if k not in ('rows','boundary')}))
