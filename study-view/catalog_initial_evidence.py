"""Current39 initial-bundle origin census, including corrected ARM64 evidence.

Audits recorded labels, raw hashes and patch scopes, not semantic truth or recall.
Native-labeled records can attach source context; those fields are not native facts.
"""
from collections import Counter
import hashlib
import json
from pathlib import Path

ROOT=Path(__file__).resolve().parent
DATA=ROOT.parent/'LLM-Native/artifacts/fse_revision'


def read(path):return json.loads(path.read_text())
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()


if __name__=='__main__':
    frame=read(ROOT/'MATRIX_FREEZE.json')['subjects'];assert len(frame)==39
    original=DATA/'generate_only_evidence_all39_v8_r5'
    scope={r['case_id']:r for r in read(original/'STRICT_BINDING_AUDIT.json')['rows']}
    origins=Counter();types=Counter();sources=Counter();rows=[]
    for case in frame:
        folder=DATA/'arm64_corrected_g04_v43' if case.startswith('G04_') else original/case
        bundle_path=folder/'csa/evidence_bundle.json';manifest_path=folder/'EVIDENCE_REPLAY_MANIFEST.json'
        bundle=read(bundle_path);manifest=read(manifest_path)
        bindings={str(bundle_path):sha(bundle_path),str(manifest_path):sha(manifest_path)}
        if case.startswith('G04_'):
            relocation=read(folder/'RELOCATION_MANIFEST.json')
            assert bindings[str(bundle_path)]==relocation['relocated_bundle_sha256']
            assert manifest['evidence_bundle_sha256']==relocation['original_bundle_sha256']
            assert bindings[str(manifest_path)]==relocation['original_collection_manifest_sha256']
            assert read(folder/'ARCHITECTURE_ADAPTER.json')['architecture']=='arm64'
            bindings[str(folder/'RELOCATION_MANIFEST.json')]=sha(folder/'RELOCATION_MANIFEST.json')
        else:assert bindings[str(bundle_path)]==manifest['evidence_bundle_sha256']
        counts=Counter();internal=[]
        for record in bundle['records']:
            provenance=record['provenance'];origin=provenance['origin']
            assert origin in ('analyzer_internal','analyzer_output','source_derived')
            origins[origin]+=1;counts[origin]+=1;types[(origin,record['type'])]+=1
            if origin=='source_derived':sources[provenance.get('artifact','unspecified').split(':',1)[0]]+=1
            if origin!='analyzer_internal':continue
            file=record['scope']['file'];function=record['scope']['function'];payload=record['semantic_payload']
            assert file in scope[case]['patch_files'] and function in scope[case]['patch_functions'].get(file,[])
            for key in ('interface','output_schema','raw_output_path','raw_output_sha256','command_sha256'):assert payload.get(key)
            assert payload['interface']=='clang-18:debug.DumpCFG+debug.DumpCallGraph'
            assert payload['output_schema']=='semweaver.csa_cfg_snapshot.v1'
            raw=Path(payload['raw_output_path'])
            if str(raw).startswith('/work/'):raw=ROOT.parent/'LLM-Native'/str(raw)[len('/work/'):]
            assert sha(raw)==payload['raw_output_sha256'];bindings[str(raw)]=sha(raw)
            internal.append({'id':record['evidence_id'],'type':record['type'],'file':file,'function':function,
                             'interface':payload['interface'],'schema':payload['output_schema'],
                             'raw_output_sha256':payload['raw_output_sha256']})
        rows.append({'case_id':case,'bundle':str(bundle_path),'origin_counts':dict(counts),
                     'internal_records':internal,'bindings':bindings})
    assert dict(origins)=={'source_derived':61,'analyzer_internal':81,'analyzer_output':51}
    result={'status':'verified_initial_bundle_catalog','subjects':39,'records':sum(origins.values()),
            'origin_counts':dict(origins),'subjects_with_static_native_records':sum(bool(r['internal_records']) for r in rows),
            'record_types':[{'origin':origin,'type':kind,'records':count} for (origin,kind),count in sorted(types.items())],
            'source_derived_artifact_prefix_counts':dict(sources),'rows':rows,
            'boundary':'Initial evidence only, separate from per-attempt dynamic availability. Record-level origins, raw digests and patch scopes audited. Native-backed records may attach source excerpts/derived context, which are not analyzer-internal facts. CFG bindings are not full symbolic ProgramState transitions or target-recall proof. Source-derived records are not all source-window fallbacks.'}
    temporary=ROOT/'INITIAL_EVIDENCE_CATALOG.tmp';temporary.write_text(json.dumps(result,indent=2))
    temporary.replace(ROOT/'INITIAL_EVIDENCE_CATALOG.json')
    print(json.dumps({k:v for k,v in result.items() if k not in ('rows','boundary')}))
