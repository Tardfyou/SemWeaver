#!/usr/bin/env python3
from __future__ import annotations
import argparse, csv, json, shutil
from pathlib import Path

ROOT=Path(__file__).resolve().parents[3]
ABL=ROOT/'artifacts/experiments/knighter/e2/ablation'
SAMPLES=ABL/'config/ablation_samples.csv'
ANALYZER_NATIVE={'semantic_slice','path_guard','state_transition','allocation_lifecycle','call_graph','data_flow','taint_flow'}
ABLATION_CONTROL_TYPE='ablation_control'

def read_json(p:Path): return json.loads(p.read_text())
def write_json(p:Path,d): p.write_text(json.dumps(d,indent=2,ensure_ascii=False)+'\n')

def _validation_records(bundle):
    return [
        r for r in (bundle.get('records',[]) or [])
        if isinstance(r,dict) and str(r.get('type','')) == 'validation_outcome'
    ]

def normalize_variant(variant):
    return 'validation_only' if str(variant or '').strip() == 'no_evidence' else str(variant or '').strip()

def ablation_control_record(variant):
    variant=normalize_variant(variant)
    return {
        'evidence_id': f'ablation_{variant}_control',
        'type': ABLATION_CONTROL_TYPE,
        'analyzer': 'experiment',
        'scope': {'repo': 'artifacts/experiments/knighter/e2/ablation', 'file': '', 'function': ''},
        'location': {'line': 0, 'column': 0},
        'semantic_payload': {
            'kind': f'ablation_{variant}',
            'request_evidence_disabled': True,
            'summary': (
                'Controlled ablation run: analyzer-derived semantic evidence is intentionally withheld. '
                'The refine agent must not use request_evidence; it must refine from the checker, patch, '
                'patch-target source context, and strict baseline validation feedback already attached.'
            ),
        },
        'provenance': {'tool': 'e2-ablation-prepare', 'artifact': variant, 'confidence': 1.0},
        'evidence_slice': {},
    }

def validation_only_bundle(source_bundle):
    records=_validation_records(source_bundle)
    records.append(ablation_control_record('validation_only'))
    return {
        'records': records,
        'missing_evidence': [],
        'collected_analyzers': ['csa'],
        'ablation_variant': 'validation_only',
        'request_evidence_disabled': True,
    }

def filter_bundle(d):
    records=[]
    for r in d.get('records',[]) or []:
        typ=str(r.get('type',''))
        analyzer=str(r.get('analyzer',''))
        eid=str(r.get('evidence_id',''))
        payload=r.get('semantic_payload',{}) if isinstance(r.get('semantic_payload'),dict) else {}
        kind=str(payload.get('kind','') or payload.get('fact_type',''))
        if typ in ANALYZER_NATIVE or kind in ANALYZER_NATIVE or analyzer == 'csa_path':
            continue
        if eid.startswith('csa_') and typ != 'validation_outcome':
            continue
        records.append(r)
    records.append(ablation_control_record('no_analyzer_native'))
    return {'records': records, 'missing_evidence': d.get('missing_evidence',[]) or [], 'collected_analyzers': d.get('collected_analyzers',[]) or ['csa']}

def scrub_shared_analysis(d, variant):
    if not isinstance(d,dict):
        return d
    patchweaver=d.get('patchweaver')
    if isinstance(patchweaver,dict):
        if variant=='validation_only':
            patchweaver['evidence_bundle']=validation_only_bundle(patchweaver.get('evidence_bundle',{}) or {})
            patchweaver['refinement_evidence_bundles']={'csa': patchweaver['evidence_bundle']}
        elif variant=='no_analyzer_native':
            patchweaver['evidence_bundle']=filter_bundle(patchweaver.get('evidence_bundle',{}) or {})
            patchweaver['refinement_evidence_bundles']={'csa': patchweaver['evidence_bundle']}
        patchweaver['ablation_control']={
            'variant': variant,
            'request_evidence_disabled': variant=='validation_only',
        }
    d['ablation_control']={
        'variant': variant,
        'request_evidence_disabled': variant=='validation_only',
    }
    return d

def scrub_result_json(p, variant):
    if not p.exists():
        return
    d=read_json(p)
    source_post=d.get('post_validation_evidence_bundle',{}) if isinstance(d.get('post_validation_evidence_bundle'),dict) else {}
    source_base=d.get('evidence_bundle',{}) if isinstance(d.get('evidence_bundle'),dict) else {}
    if variant=='validation_only':
        base=validation_only_bundle(source_base)
        post=validation_only_bundle(source_post)
    elif variant=='no_analyzer_native':
        base=filter_bundle(source_base)
        post=filter_bundle(source_post)
    else:
        return
    d['evidence_bundle']=base
    d['post_validation_evidence_bundle']=post
    d['evidence_records']=len(base.get('records',[]) or [])
    d['post_validation_evidence_records']=len(post.get('records',[]) or [])
    d['missing_evidence']=base.get('missing_evidence',[]) or []
    d['post_validation_missing_evidence']=post.get('missing_evidence',[]) or []
    d['ablation_control']={'variant': variant, 'request_evidence_disabled': variant=='validation_only'}
    write_json(p,d)

def scrub_refinement_input(p, variant):
    if not p.exists():
        return
    d=read_json(p)
    if isinstance(d.get('shared_analysis'),dict):
        d['shared_analysis']=scrub_shared_analysis(d['shared_analysis'], variant)
    artifacts=d.get('artifacts',{})
    if isinstance(artifacts,dict):
        for payload in artifacts.values():
            if not isinstance(payload,dict):
                continue
            report=payload.get('report_entry')
            if isinstance(report,dict):
                source_post=report.get('post_validation_evidence_bundle',{}) if isinstance(report.get('post_validation_evidence_bundle'),dict) else {}
                source_base=report.get('evidence_bundle',{}) if isinstance(report.get('evidence_bundle'),dict) else {}
                if variant=='validation_only':
                    report['evidence_bundle']=validation_only_bundle(source_base)
                    report['post_validation_evidence_bundle']=validation_only_bundle(source_post)
                elif variant=='no_analyzer_native':
                    report['evidence_bundle']=filter_bundle(source_base)
                    report['post_validation_evidence_bundle']=filter_bundle(source_post)
                report['evidence_records']=len(report.get('evidence_bundle',{}).get('records',[]) or [])
                report['post_validation_evidence_records']=len(report.get('post_validation_evidence_bundle',{}).get('records',[]) or [])
                report['ablation_control']={'variant': variant, 'request_evidence_disabled': variant=='validation_only'}
            payload['ablation_control']={'variant': variant, 'request_evidence_disabled': variant=='validation_only'}
    d['ablation_control']={'variant': variant, 'request_evidence_disabled': variant=='validation_only'}
    write_json(p,d)

def prepare(case_id, variant):
    variant=normalize_variant(variant)
    rows={r['case_id']:r for r in csv.DictReader(SAMPLES.open(newline=''))}
    src=ROOT/rows[case_id]['case_dir']
    dst=ABL/'runs'/variant/case_id
    if dst.exists(): shutil.rmtree(dst)
    ignore=shutil.ignore_patterns('refinements')
    shutil.copytree(src,dst,ignore=ignore)
    csa=dst/'csa'
    for name in ['evidence_bundle.json','post_validation_evidence_bundle.json']:
        p=csa/name
        if variant=='validation_only':
            write_json(p, validation_only_bundle(read_json(p)))
        elif variant=='no_analyzer_native':
            write_json(p, filter_bundle(read_json(p)))
        elif variant=='full_evidence':
            pass
        else:
            raise SystemExit(f'unknown variant: {variant}')
    if variant!='full_evidence':
        plan=dst/'patchweaver_plan.json'
        if plan.exists():
            write_json(plan, scrub_shared_analysis(read_json(plan), variant))
        scrub_result_json(csa/'result.json', variant)
        scrub_refinement_input(dst/'refinement_input.json', variant)
    # Update manifest evidence record counts for readability only.
    manifest=dst/'evidence_manifest.json'
    if manifest.exists():
        d=read_json(manifest)
        d['patch_path']='patches/commit.patch'
        d['shared_analysis_path']='patchweaver_plan.json'
        bundle=read_json(csa/'evidence_bundle.json')
        post=read_json(csa/'post_validation_evidence_bundle.json')
        art=d.setdefault('artifacts',{}).setdefault('csa',{})
        art['evidence_records']=len(bundle.get('records',[]) or [])
        art['post_validation_evidence_records']=len(post.get('records',[]) or [])
        art['ablation_variant']=variant
        art['request_evidence_disabled']=variant=='validation_only'
        d['ablation_control']={'variant': variant, 'request_evidence_disabled': variant=='validation_only'}
        write_json(manifest,d)
    print(dst)
    print('evidence', len(read_json(csa/'evidence_bundle.json').get('records',[]) or []), 'post', len(read_json(csa/'post_validation_evidence_bundle.json').get('records',[]) or []))

if __name__=='__main__':
    ap=argparse.ArgumentParser()
    ap.add_argument('--case-id', required=True)
    ap.add_argument('--variant', required=True, choices=['validation_only','no_evidence','no_analyzer_native','full_evidence'])
    a=ap.parse_args(); prepare(a.case_id,a.variant)
