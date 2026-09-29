"""Plan/export the current study without changing trials or publishing remotely.

--plan is explicitly provisional and can run during experiments. Export refuses
incomplete main/auxiliary evidence, an existing destination and changing files.
Raw negative and superseded trials remain separate from canonical aggregations.
"""
from __future__ import annotations

import argparse
from collections import Counter
import importlib.util
import io
import json
from pathlib import Path
import re
import subprocess
import sys
import tarfile

from artifact_tree_aliases import files_with_local_aliases
from verify_final_artifact import Auditor, require

PHASE = Path(__file__).resolve().parent
PROJECT = PHASE.parent
DATA = PROJECT / 'LLM-Native/artifacts/fse_revision'
spec = importlib.util.spec_from_file_location('legacy_export_utilities', PROJECT / 'build_fse2027_review_package.py')
base = importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)

SUFFIXES = base.TEXT_SUFFIXES | {'.stdout', '.stderr', '.cmake', '.svg', '.rst'}
SKIP = (base.SKIP_PARTS - {'scan-reports', 'scan-reports-0'}) | {'codeql_db', 'databases', 'db-cpp', 'external',
                           'kernel', 'kernels', 'linux', 'versions-cache', '.travis.yml', 'english-main-prompts'}
DATA_TREES = (
    'generate_only_materialized_v1', 'generate_only_evidence_all39_v8_r5',
    'arm64_corrected_g04_v43', 'generate_only_screen_v1',
    'generate_only_knighter_gpt6luna_high_v8', 'scanfix_v26_corrected_summary_v1',
    'motivating_g21_quality_v23_r4', 'motivating_g21_perturbations_v1',
    'motivating_g21_confirmation_v1', 'motivating_g21_transfer_v1',
    'motivating_g21_selection_20260927', 'backend_cost_20260927_v1',
    'backend_cost_summary_20260927_v1', 'backend_integration_audit_20260927_v1',
    'e2_imagemagick_development_v8',
)
REQUIRED_DATA = {'generate_only_materialized_v1', 'generate_only_evidence_all39_v8_r5',
                 'arm64_corrected_g04_v43', 'generate_only_screen_v1',
                 'generate_only_knighter_gpt6luna_high_v8', 'scanfix_v26_corrected_summary_v1',
                 'motivating_g21_quality_v23_r4', 'backend_cost_20260927_v1',
                 'backend_cost_summary_20260927_v1', 'backend_integration_audit_20260927_v1',
                 'e2_imagemagick_development_v8'}
SOURCE_VERSIONS = (10, 11, 19, 20, 21, 22, 23, 26, 40, 41, 42, 43)


def version_repo(version):
    name = 'SemWeaver-example-v23' if version == 23 else f'SemWeaver-v{version}'
    return PROJECT / 'LLM-Native' / name


def public_path(path):
    return Path('project') / path.relative_to(PROJECT)


def corrected_starting_roots(summary, data_root=DATA, project_root=PROJECT):
    roots = []
    for row in summary['starting']['rows']:
        recorded = Path(row['validation_path'])
        validation = recorded if recorded.is_absolute() else data_root / recorded
        validation = validation.resolve(strict=True)
        require(validation.is_relative_to(project_root.resolve()), 'Starting validation escapes project')
        require(validation.is_file(), 'Missing corrected starting validation')
        require(base.sha_file(validation) == row['validation_sha256'], 'Corrected starting receipt drift')
        roots.append(validation.parent)
    return list(dict.fromkeys(roots))


def source_state(repo, *, require_clean):
    head = subprocess.run(['git', '-C', str(repo), 'rev-parse', 'HEAD'],
                          capture_output=True, text=True, check=True).stdout.strip()
    diff = subprocess.run(['git', '-C', str(repo), 'diff', 'HEAD', '--binary'],
                          capture_output=True, check=True).stdout
    changed = subprocess.run(['git', '-C', str(repo), 'diff', '--name-only', 'HEAD'],
                             capture_output=True, text=True, check=True).stdout.splitlines()
    require(not require_clean or not changed, 'Dirty selected/footprint execution source')
    return {'head': head, 'tracked_dirty': bool(changed), 'dirty_paths': changed,
            'working_diff_sha256': base.sha_bytes(diff), 'archive': 'committed_HEAD',
            'boundary': 'Working edits are preserved separately; the committed archive is not asserted to contain those edits.'}, diff


def inventory():
    """Logical paths mirror project layout; aliases retain their own identities."""
    files = {}
    roots = [PHASE, PROJECT / 'native-state-20260928', PROJECT / 'native-trace-20260928']
    for name in DATA_TREES:
        path = DATA / name
        if path.is_dir():
            roots.append(path)
        else:
            require(name not in REQUIRED_DATA, f'Missing evidence tree: {name}')
    # Every actual automatically edited motivating-example ancestor, including
    # the initial Flash execution. Do not export only the successful r4 leaf.
    selection = base.load(PROJECT / 'review-revision-20260927/FINAL_SELECTION.json')
    roots.extend(Path(r['run']) for r in selection['model_lineage'])
    # Corrected starting receipts refer to raw validations outside the summary
    # tree. Retain their complete logs/reports, including normal zero-report
    # scans; a copied RESULT receipt alone does not close raw evidence.
    starting = base.load(DATA / 'scanfix_v26_corrected_summary_v1/RESULT.json')
    roots.extend(corrected_starting_roots(starting))
    roots = list(dict.fromkeys(roots))
    for root in roots:
        require(root.is_dir(), f'Missing phase: {root.name}')
        for logical, actual in files_with_local_aliases(root, SKIP):
            if logical.suffix.lower() not in SUFFIXES:
                continue
            # Credentials/backups are never selected, even if introduced later.
            require(not any(part.startswith('.') for part in logical.parts), 'Hidden evidence file requires privacy review')
            relative = public_path(root / logical)
            require(actual.stat().st_size <= base.MAX_FILE_BYTES, f'Oversize evidence: {relative}')
            if relative in files:
                require(files[relative] == actual, 'Conflicting logical evidence path')
                continue
            files[relative] = actual
    # Root experiment drivers, never arbitrary home/config/auth files.
    for path in sorted(PROJECT.glob('*.py')):
        files[public_path(path)] = path
    for version, folder in ((v, f) for v in (22, 43) for f in ('src', 'config')):
        # Source is separately archived, but direct logical source paths are
        # necessary to resolve recorded hash bindings without rewriting them.
        source = PROJECT / f'LLM-Native/SemWeaver-v{version}' / folder
        for logical, actual in files_with_local_aliases(source, SKIP):
            if logical.suffix.lower() in SUFFIXES:
                files[public_path(source / logical)] = actual
    footprint = base.load(DATA / 'backend_integration_audit_20260927_v1/RESULT.json')
    for row in footprint['rows']:
        path = PROJECT / row['path'] if row['category'] == 'external_e2_protocol_and_tests' else PROJECT / 'LLM-Native/SemWeaver-v22' / row['path']
        require(path.is_file() and base.sha_file(path) == row['sha256'], 'Backend-audit source binding drift')
        files[public_path(path)] = path
    # Preserve every bound raw/native input, including probe fixtures and
    # executed experiment helpers that are outside src/config directories.
    for name in ('INITIAL_EVIDENCE_CATALOG.json', 'NATIVE_EVIDENCE_AVAILABILITY_STATUS.json'):
        ledger = base.load(PHASE / name)
        for row in ledger['rows']:
            for recorded, expected in row['bindings'].items():
                path = Path(recorded)
                actual = path.resolve(strict=True)
                require(path.is_relative_to(PROJECT) and actual.is_relative_to(PROJECT), 'Bound evidence escapes project')
                require(actual.is_file() and base.sha_file(actual) == expected, 'Bound evidence input changed')
                files[public_path(path)] = actual
    review = PROJECT / 'review-revision-20260927'
    for name in ('FINAL_SELECTION.json', 'MOTIVATING_EXAMPLE_DRAFT.tex',
                 'render_motivating_example.py', 'BACKEND_INTEGRATION_COST.md'):
        files[public_path(review / name)] = review / name
    for path in sorted((review / 'motivating-example/final-legible').iterdir()):
        if path.is_file() and path.suffix in {'.svg', '.pdf', '.png', '.md'}:
            files[public_path(path)] = path
    codeql = PROJECT / 'LLM-Native/experiment_source/e2'
    for relative in (
        'runs/vul4c_cwe416_imagemagick_cve201712877/primary/codeql/20260423_151031/codeql/ImageListDeletedAliasUseAfterFree.ql',
        'datasets/curated/vul4c_cwe416_imagemagick_cve201712877/patches/fix.patch',
    ):
        path = codeql / relative
        require(path.is_file(), 'Missing automatic CodeQL input lineage')
        files[public_path(path)] = path
    for side in ('vulnerable', 'fixed'):
        path = DATA / f'e2_imagemagick_development_v8/codeql_db/416_{side}/codeql-database.yml'
        require(path.is_file(), 'Missing retained CodeQL database metadata')
        files[public_path(path)] = path
    linux = PROJECT / 'LLM-Native/SemWeaver/artifacts/external/linux'
    for relative in ('COPYING', 'LICENSES/preferred/GPL-2.0'):
        path = linux / relative
        require(path.is_file(), 'Missing Linux upstream notice')
        files[Path('licenses') / ('Linux-COPYING.txt' if relative == 'COPYING' else 'Linux-GPL-2.0.txt')] = path
    upstream = PROJECT / 'LLM-Native/SemWeaver-v43/experiments/knighter/baseline/LICENSE'
    require(upstream.is_file(), 'Missing KNighter upstream license')
    files[Path('licenses/KNighter-Apache-2.0.txt')] = upstream
    return files


def describe(files):
    counts, sizes = Counter(), Counter()
    for name, path in files.items():
        group = '/'.join(name.parts[:2]) if len(name.parts) > 2 else 'project/root-drivers'
        if name.parts[:2] == ('project', 'LLM-Native'):
            group = '/'.join(name.parts[:3])
        counts[group] += 1
        sizes[group] += path.stat().st_size
    return {'status': 'provisional_export_inventory_not_a_release', 'files': len(files),
            'bytes': sum(sizes.values()), 'groups': [
                {'path': k, 'files': counts[k], 'bytes': sizes[k]} for k in sorted(counts)],
            'boundary': 'Live inventory only. Source archives, manuscript and final privacy/review gates remain separate.'}


def verify_export_inputs(files, crosswalk):
    originals = {row['path']: row['original_sha256'] for row in crosswalk}
    checked = {}
    for name, source in files.items():
        key = source.resolve(strict=True)
        if key not in checked:
            checked[key] = base.sha_file(source)
        require(originals.get(name.as_posix()) == checked[key], 'Export input changed after copying')


class CurrentPackage(base.Package):
    def __init__(self, root):
        super().__init__(root)
        self.crosswalk = []

    def put(self, relative, data, *, text=True):
        # Known key formats are a refusal gate, not automatic credential removal.
        if text:
            require(not re.search(rb'(?:sk-[A-Za-z0-9_-]{20,}|olp_[A-Za-z0-9]{20,}|\b[a-f0-9]{32}\.[A-Za-z0-9]{16,}\b)', data),
                    'Credential-shaped material requires private review')
        rewritten, labels = base.redact(data) if text else (data, [])
        if text:
            for label, pattern, replacement in (
                ('author_repository', rb'https?://github\.com/anonymous-user/SemWeaver(?:\.git)?',
                 b'https://anonymous.4open.science/r/SemWeaver-FD1E/'),
                ('private_overleaf_project', rb'https?://(?:git@)?git\.overleaf\.com/[a-f0-9]{24}',
                 b'https://overleaf.example.invalid/anonymous-project'),
                ('local_author_identifier', rb'\b(?:anonymous-user|anonymous-user|anonymous-user)\b', b'anonymous-user'),
            ):
                changed = re.sub(pattern, replacement, rewritten)
                if changed != rewritten:
                    labels.append(label)
                    rewritten = changed
        super().put(relative, rewritten, text=False)
        if labels:
            self.redactions.append({'path': Path(relative).as_posix(),
                                    'original_sha256': base.sha_bytes(data),
                                    'redacted_sha256': base.sha_bytes(rewritten),
                                    'replacements': labels})
        self.crosswalk.append({'path': Path(relative).as_posix(),
                               'original_sha256': base.sha_bytes(data),
                               'public_sha256': base.sha_file(self.root / relative)})

    def copy(self, source, relative):
        require(source.is_file() and not source.is_symlink(), 'Invalid export source')
        before = base.sha_file(source)
        raw = source.read_bytes()
        require(before == base.sha_bytes(raw) == base.sha_file(source), 'Evidence changed during export')
        self.put(relative, raw, text=source.suffix.lower() in SUFFIXES or
                 source.name in {'COPYING', 'LICENSE', 'GPL-2.0', '.gitignore', '.env.example'})

    def git_archive(self, repo, relative, revision='HEAD', *, merge_identical=False):
        archive = subprocess.run(['git', '-C', str(repo), 'archive', revision],
                                 capture_output=True, check=True).stdout
        existing = {r['path']: r for r in self.crosswalk}
        with tarfile.open(fileobj=io.BytesIO(archive), mode='r:') as handle:
            for member in handle:
                if not member.isfile():
                    continue
                name = Path(member.name)
                require(not name.is_absolute() and '..' not in name.parts, 'Unsafe source archive member')
                stream = handle.extractfile(member)
                require(stream is not None, 'Unreadable source archive member')
                raw = stream.read()
                target = Path(relative) / name
                if (self.root / target).exists():
                    row = existing.get(target.as_posix())
                    require(merge_identical and row is not None and row['original_sha256'] == base.sha_bytes(raw)
                            and row['public_sha256'] == base.sha_file(self.root / target), 'Conflicting source/archive bytes')
                    continue
                self.put(target, raw, text=name.suffix.lower() in SUFFIXES or name.name in
                         {'LICENSE', 'COPYING', '.gitignore', '.env.example'})


README = """# SemWeaver review artifact

This package preserves recorded automated experiments, including negative and
superseded trials. Canonical tables do not pool these separate configurations.
No project-level license is granted; upstream notices remain applicable.

Run the offline verification (Python 3.10+; no API keys or network required):

```sh
python3 project/final-native-20260928/verify_final_artifact.py .
```

Start at `project/final-native-20260928/FINAL39_SUMMARY.json` (39 subjects),
`AUXILIARY_SUMMARY.json` (12 subjects with three paired decodes; 12 subjects
across three native model configurations), and `main39-tables/`. These measure
patch-version warning positivity and report burden, not independently adjudicated
target recall, deployment precision or unseen generalization.

`EVIDENCE_ORIGINS.md` defines the actual interfaces and 193 initial records;
dynamic availability is reported separately. `MOTIVATING_EXAMPLE_DECISION.md`
distinguishes the separately budgeted, fixture-guided automated illustration
from the main one-response checker. Neither example proves a causal native
evidence advantage by itself. The retention guard is native-only.

All new explanatory entry documentation is English. Historical raw model,
tool and administrative notes remain in their original language and are not
English translations. Private machine paths are replaced with virtual artifact
paths; `ORIGINAL_PUBLIC_HASHES.json` maps original hashes to exported bytes.
Rebuildable objects, virtual environments, Linux trees, CodeQL databases and
private credentials/backups are deliberately excluded. Source versions and
drivers are retained; deterministic reruns require the pinned tools and subject
checkouts. Model calls cannot be promised to reproduce byte-identical replies.

This byte/aggregation verifier does not certify manuscript agreement, citation
authenticity, licenses, semantic target labels or final anonymous publication.
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--plan', action='store_true')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--distribution', type=Path, help='Optionally build a new lossless compressed distribution')
    args = parser.parse_args()
    behavior_files = [Path(__file__), Path(base.__file__), PHASE / 'verify_final_artifact.py',
                      PHASE / 'artifact_tree_aliases.py', PHASE / 'compressed_artifact.py']
    behavior_hashes = {path: base.sha_file(path) for path in behavior_files}
    source_states, source_diffs = {}, {}
    # Fail before copying large evidence. Historical worktree edits are not
    # committed/reset or silently presented as the committed execution tree.
    for version in SOURCE_VERSIONS:
        repo = version_repo(version)
        if repo.is_dir():
            state, diff = source_state(repo, require_clean=version in (22, 43))
            state['original_project_path'] = public_path(repo).as_posix()
            source_states[str(version)], source_diffs[str(version)] = state, diff
        else:
            require(version not in (22, 43), 'Missing required selected/footprint source')
            source_states[str(version)] = {'state': 'unavailable_local_tree',
                'boundary': 'No full archive asserted. Recorded checker/trace inputs and current replay remain separately bound.'}
    files = inventory()
    if args.plan:
        print(json.dumps(describe(files), indent=2))
        return
    require(args.output is not None, 'Export requires --output')
    output = args.output.resolve()
    require(not output.exists(), 'Refusing existing export destination')
    require(not output.is_relative_to(PHASE), 'Export cannot be inside selected evidence')
    if args.distribution:
        distribution = args.distribution.resolve()
        require(not distribution.exists() and not distribution.is_relative_to(output) and
                not distribution.is_relative_to(PHASE), 'Unsafe/existing distribution destination')
    auditor = Auditor(PHASE, PROJECT)
    main_data = auditor.main()
    auditor.auxiliary(main_data)  # Never publish a partial matrix.
    auditor.backend()
    auditor.evidence(main_data)
    auditor.raw_csa_counts()
    require(base.committed_head(PROJECT / 'LLM-Native/SemWeaver-v43') == main_data['source_revision'],
            'Selected source revision drift')
    output.mkdir(parents=True)
    package = CurrentPackage(output)
    for name, source in sorted(files.items()):
        package.copy(source, name)
    revisions = {}
    for version in SOURCE_VERSIONS:
        repo = version_repo(version)
        if repo.is_dir():
            state = source_states[str(version)]
            revisions[str(version)] = state['head']
            package.git_archive(repo, f'source-versions/SemWeaver-v{version}', state['head'])
            if state['tracked_dirty']:
                package.put(f'historical-working-tree/SemWeaver-v{version}/tracked-working.diff',
                            source_diffs[str(version)])
                for changed in state['dirty_paths']:
                    source = repo / changed
                    require(source.is_file() and not source.is_symlink(), 'Unsupported historical deletion/link requires review')
                    package.copy(source, f'historical-working-tree/SemWeaver-v{version}/files/{changed}')
            if version in (22, 43):
                # Complete import/entry/test trees at the actual recorded
                # project paths, not just partial src/config plus an appendix.
                package.git_archive(repo, f'project/LLM-Native/SemWeaver-v{version}', state['head'], merge_identical=True)
            after = source_state(repo, require_clean=version in (22, 43))[0]
            after['original_project_path'] = public_path(repo).as_posix()
            require(after == state,
                    'Historical/selected source changed during export')
    verify_export_inputs(files, package.crosswalk)
    require(all(base.sha_file(path) == expected for path, expected in behavior_hashes.items()),
            'Export implementation changed while running')
    package.put('README.md', README.encode())
    package.put('SOURCE_VERSION_STATUS.json', (json.dumps(source_states, indent=2)+'\n').encode())
    # Snapshot the mapping before writing itself/manifest: those generated
    # containers are protected by the package manifest, not recursive hashes.
    crosswalk = list(package.crosswalk)
    package.put('ORIGINAL_PUBLIC_HASHES.json', (json.dumps({'schema_version': 1, 'files': crosswalk}, indent=2)+'\n').encode())
    package.finish({'study_version': 'final39-v43-focused12', 'source_revisions': revisions,
                    'export_behavior_sha256': {public_path(p).as_posix(): h for p, h in behavior_hashes.items()},
                    'main_subjects': 39, 'additional_ablation_cells': 48, 'model_configuration_cells': 36,
                    'status': 'exported_recorded_evidence_pending_release_review'})
    subprocess.run([sys.executable, str(output / public_path(PHASE / 'verify_final_artifact.py')), str(output)], check=True)
    if args.distribution:
        from compressed_artifact import pack, verify_distribution
        pack(output, distribution)
        verify_distribution(distribution)
    print(json.dumps({'status': 'exported_and_offline_verified', 'output': str(output),
                      'publication_ready': False, 'files': len(crosswalk)}))


if __name__ == '__main__':
    main()
