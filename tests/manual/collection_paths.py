"""Locate shared runtime resources without depending on the caller's cwd.

Development modules live in tests/manual; delivered modules live in
dist/RelinkStudio/collection. Model files and user evidence remain in the
project directory. Merely resolving a path performs no desktop interaction.
"""
import os
from pathlib import Path


MARKERS = ('.tools/ocr-runtime', 'tests/manual', 'dist')


def _valid_project(path):
    return path.is_dir() and all((path / name).is_dir() for name in MARKERS)


def project_root(module_file, *, environ=None):
    environment = os.environ if environ is None else environ
    explicit = environment.get('RELINK_PROJECT_ROOT')
    if explicit is not None:
        candidate = Path(explicit)
        if not candidate.is_absolute():
            raise ValueError('COLLECTION_PROJECT_ROOT_ABSOLUTE_REQUIRED')
        candidate = candidate.resolve()
        if not _valid_project(candidate):
            raise ValueError('COLLECTION_PROJECT_ROOT_MARKERS_MISSING')
        return candidate
    module = Path(module_file).resolve()
    candidates = []
    if module.parent.name == 'manual' and module.parent.parent.name == 'tests':
        candidates.append(module.parents[2])
    if (module.parent.name == 'collection' and module.parent.parent.name == 'RelinkStudio'
            and module.parent.parent.parent.name == 'dist'):
        candidates.append(module.parents[3])
    for candidate in candidates:
        if _valid_project(candidate):
            return candidate
    raise ValueError('COLLECTION_PROJECT_ROOT_NOT_FOUND')
