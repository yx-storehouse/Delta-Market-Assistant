"""Copy the collection CLI's static local-import closure, never its model files.

Imports are inspected with ast.walk, including imports inside functions; no
application module is imported or executed while preparing this package.
"""
from __future__ import annotations

import argparse
import ast
import hashlib
import json
from pathlib import Path
import shutil


ENTRY = 'run_collection_observed.py'
# The program's run hotkey starts this one; packaged whenever it exists.
OPTIONAL_ENTRIES = ('run_collection_hotkey.py', 'run_purchase_rehearsal.py', 'run_purchase_probe.py',
                    'run_purchase_countdown_probe.py', 'run_purchase_preentry_probe.py', 'run_purchase_timed_buy.py',
                    'run_purchase_toast_probe.py')
MANIFEST = 'runtime_manifest.json'
DEFAULT_BANK = 'artifacts/collection_scroll_speed/calibration_bank.json'


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def import_closure(source_dir, entry=ENTRY, optional=OPTIONAL_ENTRIES):
    source_dir = Path(source_dir).resolve()
    entries = [entry] + [name for name in optional if (source_dir / name).is_file()]
    if any(Path(name).name != name or not name.endswith('.py') for name in entries):
        raise ValueError('COLLECTION_RUNTIME_ENTRY_NAME')
    pending, found = list(entries), set()
    while pending:
        name = pending.pop()
        if name in found:
            continue
        path = source_dir / name
        if not path.is_file():
            raise ValueError('COLLECTION_RUNTIME_SOURCE_MISSING:' + name)
        found.add(name)
        tree = ast.parse(path.read_text(encoding='utf-8-sig'), filename=str(path))
        for node in ast.walk(tree):
            if isinstance(node, ast.Import):
                modules = [item.name.split('.')[0] for item in node.names]
            elif isinstance(node, ast.ImportFrom):
                if node.level:
                    raise ValueError('COLLECTION_RUNTIME_RELATIVE_IMPORT:' + name)
                modules = [node.module.split('.')[0]] if node.module else []
            else:
                continue
            for module in modules:
                local = source_dir / (module + '.py')
                if local.is_file() and local.name not in found:
                    if local.name.startswith('test_'):
                        raise ValueError('COLLECTION_RUNTIME_DEPENDS_ON_TEST:' + local.name)
                    pending.append(local.name)
    return sorted(found)


def managed_child(directory, name):
    if Path(name).name != name or not name.endswith('.py'):
        raise ValueError('COLLECTION_RUNTIME_MANAGED_NAME')
    path = directory / name
    if path.is_symlink() or path.resolve().parent != directory.resolve():
        raise ValueError('COLLECTION_RUNTIME_PATH_ESCAPED')
    return path


def calibration_dependencies(root):
    """Record the external default bank and its exact JSON dependency closure.

    This is a project-bound package, so calibration evidence keeps its original
    artifacts location. Structural/path/hash checks here do not replace the
    runtime loader's measured-evidence semantic checks.
    """
    root = Path(root).resolve()
    bank_path = root / DEFAULT_BANK
    result = dict(default_bank_path=DEFAULT_BANK, available=bank_path.is_file(),
                  copied_files=0, semantic_validation='published_runtime_loader', files={})
    if not bank_path.exists():
        return result

    def read(relative, role, limit, expected=None):
        item = Path(relative)
        path = (root / item).resolve()
        if (item.is_absolute() or not item.parts or item.parts[0] != 'artifacts'
                or '..' in item.parts or not path.is_relative_to(root / 'artifacts')
                or path.suffix != '.json' or not path.is_file()):
            raise ValueError('COLLECTION_PACKAGE_BANK_DEPENDENCY_PATH:' + str(relative))
        raw = path.read_bytes()
        if not 0 < len(raw) <= limit:
            raise ValueError('COLLECTION_PACKAGE_BANK_DEPENDENCY_SIZE:' + str(relative))
        digest = hashlib.sha256(raw).hexdigest()
        if expected is not None and digest != expected:
            raise ValueError('COLLECTION_PACKAGE_BANK_DEPENDENCY_HASH:' + str(relative))
        name = path.relative_to(root).as_posix()
        result['files'][name] = dict(role=role, bytes=len(raw), sha256=digest)
        try:
            return json.loads(raw.decode('utf-8-sig'))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise ValueError('COLLECTION_PACKAGE_BANK_DEPENDENCY_JSON:' + str(relative)) from error

    bank = read(DEFAULT_BANK, 'bank', 512 * 1024)
    if (not isinstance(bank, dict) or bank.get('schema') != 'collection-scroll-calibration-bank-v1'
            or not isinstance(bank.get('profiles'), list) or not 1 <= len(bank['profiles']) <= 64):
        raise ValueError('COLLECTION_PACKAGE_BANK_SCHEMA')
    seen = set()
    for entry in bank['profiles']:
        if (not isinstance(entry, dict) or not isinstance(entry.get('path'), str)
                or not isinstance(entry.get('sha256'), str) or len(entry['sha256']) != 64
                or any(char not in '0123456789abcdef' for char in entry['sha256'])):
            raise ValueError('COLLECTION_PACKAGE_BANK_ENTRY')
        identity = (root / entry['path']).resolve()
        if identity in seen:
            raise ValueError('COLLECTION_PACKAGE_BANK_DUPLICATE')
        seen.add(identity)
        profile = read(entry['path'], 'profile', 512 * 1024, entry['sha256'])
        source = profile.get('source') if isinstance(profile, dict) else None
        if (not isinstance(profile, dict) or profile.get('schema') != 'collection-scroll-calibration-v1'
                or not isinstance(source, dict) or not isinstance(source.get('record_path'), str)
                or not isinstance(source.get('record_sha256'), str) or len(source['record_sha256']) != 64):
            raise ValueError('COLLECTION_PACKAGE_BANK_PROFILE_SCHEMA')
        evidence = read(source['record_path'], 'measurement', 8 * 1024 * 1024, source['record_sha256'])
        if (not isinstance(evidence, dict)
                or evidence.get('schema') != 'collection-scroll-calibration-measurement-v1'):
            raise ValueError('COLLECTION_PACKAGE_BANK_MEASUREMENT_SCHEMA')
    return result


def package_runtime(root, destination):
    root, destination = Path(root).resolve(), Path(destination).resolve()
    if destination != root / 'dist/RelinkStudio':
        raise ValueError('COLLECTION_RUNTIME_DESTINATION')
    source = root / 'tests/manual'
    names = import_closure(source)
    external_bank = calibration_dependencies(root)
    runtime = destination / 'collection'
    if runtime.is_symlink():
        raise ValueError('COLLECTION_RUNTIME_DIRECTORY_REPARSE')
    runtime.mkdir(parents=True, exist_ok=True)
    previous = {}
    manifest_path = runtime / MANIFEST
    if manifest_path.exists():
        value = json.loads(manifest_path.read_text(encoding='utf-8-sig'))
        if value.get('schema') != 'collection-runtime-manifest-v1':
            raise ValueError('COLLECTION_RUNTIME_PRIOR_MANIFEST')
        previous = value['files']
    remove = []
    for name, entry in previous.items():
        path = managed_child(runtime, name)
        if name not in names and path.exists():
            if sha(path) != entry['sha256']:
                raise ValueError('COLLECTION_RUNTIME_STALE_FILE_CHANGED:' + name)
            remove.append(path)
    # Freeze all source metadata before any copy or removal.
    files = {}
    for name in names:
        managed_child(runtime, name)
        path = source / name
        files[name] = dict(source_path=path.relative_to(root).as_posix(),
                           bytes=path.stat().st_size, sha256=sha(path))
    for name, entry in files.items():
        path = managed_child(runtime, name)
        if not path.is_file() or sha(path) != entry['sha256']:
            shutil.copy2(source / name, path)
        if sha(path) != entry['sha256']:
            raise ValueError('COLLECTION_RUNTIME_COPY_HASH:' + name)
    for path in remove:
        path.unlink()
    manifest = dict(schema='collection-runtime-manifest-v1', entrypoint=ENTRY,
                    entrypoints=[ENTRY] + [name for name in OPTIONAL_ENTRIES if name in files],
                    dependency_method='recursive_ast_imports_including_function_bodies',
                    project_root_resolution='collection_paths.project_root; optional absolute RELINK_PROJECT_ROOT',
                    external_models='.tools/ocr-models in resolved project root; not copied',
                    external_python='.tools/ocr-runtime/Scripts/python.exe in resolved project root',
                    external_calibration_bank=external_bank,
                    model_files_copied=0, module_count=len(files), files=files)
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    if json.loads(manifest_path.read_text(encoding='utf-8')) != manifest:
        raise ValueError('COLLECTION_RUNTIME_MANIFEST_READBACK')
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--destination', type=Path)
    parser.add_argument('--list-only', action='store_true')
    args = parser.parse_args()
    root = args.root.resolve()
    if args.list_only:
        print(json.dumps(import_closure(root / 'tests/manual'), ensure_ascii=False, indent=2))
        return 0
    manifest = package_runtime(root, args.destination or root / 'dist/RelinkStudio')
    print('COLLECTION_RUNTIME_PACKAGE=PASS; modules=' + str(manifest['module_count'])
          + '; copied_model_files=0; dependency_scan=AST; application_imports_executed=0')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
