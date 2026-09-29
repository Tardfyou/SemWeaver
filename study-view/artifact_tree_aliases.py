"""Enumerate evidence files with local aliases flattened, rejecting escapes.

The existing exporter skips directory links and refuses file links. Adaptive
probe receipts use both. Resolve only inside the given evidence tree; no caches
or external worktrees are followed. The caller chooses which files to export.
"""
from pathlib import Path


def files_with_local_aliases(source,skip_parts=()):
    source=Path(source).resolve(strict=True)
    assert source.is_dir()
    def visit(directory,relative,ancestors):
        actual=directory.resolve(strict=True)
        if not actual.is_relative_to(source):raise ValueError('Evidence alias escapes selected tree')
        if actual in ancestors:raise ValueError('Cyclic evidence directory alias')
        for entry in sorted(directory.iterdir()):
            if entry.name in skip_parts:continue
            target=entry.resolve(strict=True)
            if not target.is_relative_to(source):raise ValueError('Evidence alias escapes selected tree')
            logical=relative/entry.name
            if target.is_dir():
                yield from visit(entry,logical,ancestors|{actual})
            elif target.is_file():
                yield logical,target
    yield from visit(source,Path(),set())


def export_tree_with_aliases(package,source,relative,text_suffixes,skip_parts=()):
    for logical,actual in files_with_local_aliases(source,skip_parts):
        if logical.suffix.lower() in text_suffixes:
            package.copy(actual,Path(relative)/logical)
