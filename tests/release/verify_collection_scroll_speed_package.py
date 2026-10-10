"""Offline package/rollback evidence for the Python collection-scroll update.

The frozen baseline is never rebuilt or modified. An unchanged native EXE is
valid: the changed Python module must match the actually delivered CLI copy.
This verifier does not launch visible UI, capture the game, or dispatch input.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
from datetime import datetime
import json
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import unittest
import uuid

import verify_collection_full_package as shared
import verify_pr12a_package as h
from package_collection_runtime import calibration_dependencies, import_closure


ROOT = Path(__file__).resolve().parents[2]
TX = ROOT / 'artifacts/collection_scroll_speed'
RELEASE = ROOT / 'dist/RelinkStudio'
BASELINE = TX / 'baseline/release'
EXPECTED_BASELINE_EXE_SHA256 = '88a268a5350643f6ddb24b2e8e4f8209340456a030faf3e871920093f3fc1255'
ARTIFACTS = ('MODIFIED_FILE.py', 'DIFF_FILE', 'VERIFICATION.txt', 'ROLLBACK.sh')
REQUIRED = (*shared.REQUIRED, 'BASELINE_SCROLL_TESTS', 'MODIFIED_CLI_HELP',
            'MODIFIED_RUNTIME_IMPORTS', 'MODIFIED_RUNTIME_RESOURCES', 'MODIFIED_RUNTIME_BANK',
            'MODIFIED_PACKAGING_TESTS', 'PACKAGE_TOOL_TESTS', 'RESTORED_SCROLL_TESTS')


@contextmanager
def shared_transaction():
    """Bind mutable helper globals temporarily; do not alter frozen defaults.

    check_frozen/source_diff have definition-time ROOT/TX default values. Every
    call in this verifier supplies those arguments explicitly instead of
    mutating __defaults__ or relying on reassignment of module globals.
    """
    saved = [(module, name, getattr(module, name)) for module in (shared, h)
             for name in ('TX', 'BASELINE', 'RELEASE')]
    try:
        for module in (shared, h):
            module.TX, module.BASELINE, module.RELEASE = TX, BASELINE, RELEASE
        yield
    finally:
        for module, name, value in saved:
            setattr(module, name, value)


def frozen_sources():
    sources = shared.check_frozen(ROOT, TX)
    original = shared.read_json(TX / 'baseline_release_hashes.json')
    if original.get('RelinkStudio.exe') != EXPECTED_BASELINE_EXE_SHA256:
        raise ValueError('UNEXPECTED_SCROLL_BASELINE_EXE_HASH')
    return sources


def assert_changed_module(root, tx, release, source_name, runtime_name):
    """Compare frozen bytes, current source, and delivered runtime independently."""
    sources = shared.read_json(tx / 'baseline_source_hashes.json')
    if source_name not in sources:
        raise ValueError('PRIMARY_MODULE_NOT_IN_FROZEN_SOURCE')
    before = shared.confined(tx / 'baseline/source', source_name)
    current = shared.confined(root, source_name)
    delivered = shared.confined(release, runtime_name)
    if shared.sha(before) != sources[source_name]:
        raise ValueError('PRIMARY_MODULE_BASELINE_HASH_MISMATCH')
    if current.suffix != '.py' or delivered.suffix != '.py':
        raise ValueError('PRIMARY_MODULE_MUST_BE_PYTHON')
    old_hash, new_hash, delivered_hash = map(shared.sha, (before, current, delivered))
    if new_hash == old_hash:
        raise ValueError('PRIMARY_PYTHON_MODULE_UNCHANGED')
    if delivered_hash != new_hash:
        raise ValueError('DELIVERED_PYTHON_MODULE_DIFFERS_FROM_SOURCE')
    return dict(source_path=source_name, runtime_path=runtime_name,
                baseline_sha256=old_hash, modified_sha256=new_hash,
                delivered_sha256=delivered_hash)


def check_package(args):
    frozen_sources()
    manifest = shared.read_json(RELEASE / 'file_manifest.json')['files']
    actual = {p.relative_to(RELEASE).as_posix() for p in RELEASE.rglob('*') if p.is_file()}
    if actual != set(manifest) | {'file_manifest.json'}:
        raise ValueError('PACKAGE_FILE_SET_MISMATCH')
    for name, entry in manifest.items():
        path = shared.confined(RELEASE, name)
        if shared.sha(path) != entry['sha256'] or path.stat().st_size != entry['bytes']:
            raise ValueError('PACKAGE_FILE_MISMATCH: ' + name)
    for name in ('Qt6Sql.dll', 'sqldrivers/qsqlite.dll', 'platforms/qwindows.dll',
                 'platforms/qoffscreen.dll', 'vision/windows_ocr_worker.ps1',
                 args.runtime_entry, args.runtime_module):
        if name not in manifest:
            raise ValueError('MISSING_PACKAGED_DEPENDENCY: ' + name)
    if shared.sha(RELEASE / 'RelinkStudio.exe') != shared.sha(args.build_dir / 'RelinkStudio.exe'):
        raise ValueError('BUILD_AND_PACKAGE_EXECUTABLE_DIFFER')
    module = assert_changed_module(ROOT, TX, RELEASE, args.source_module, args.runtime_module)
    runtime = shared.confined(RELEASE, args.runtime_module).parent
    runtime_sources = {}
    for path in runtime.rglob('*'):
        if not path.is_file():
            continue
        if path.suffix.lower() == '.onnx':
            raise ValueError('DUPLICATED_MODEL_IN_COLLECTION_RUNTIME')
        if path.suffix != '.py':
            continue
        # Flat copied runtime modules have an auditable source counterpart.
        source = ROOT / 'tests/manual' / path.name
        if source.is_file():
            if shared.sha(path) != shared.sha(source):
                raise ValueError('RUNTIME_MODULE_DIFFERS_FROM_SOURCE: ' + path.name)
            runtime_sources[path.relative_to(RELEASE).as_posix()] = dict(
                source_path=source.relative_to(ROOT).as_posix(), sha256=shared.sha(source))
    if args.runtime_module not in runtime_sources:
        raise ValueError('PRIMARY_RUNTIME_MODULE_NOT_SOURCE_MAPPED')
    runtime_manifest = shared.read_json(runtime / 'runtime_manifest.json')
    expected_names = import_closure(ROOT / 'tests/manual')
    if (runtime_manifest.get('schema') != 'collection-runtime-manifest-v1'
            or runtime_manifest.get('model_files_copied') != 0
            or runtime_manifest.get('module_count') != len(expected_names)
            or runtime_manifest.get('entrypoint') != Path(args.runtime_entry).name
            or set(runtime_manifest.get('files', {})) != set(expected_names)):
        raise ValueError('RUNTIME_IMPORT_CLOSURE_MANIFEST_MISMATCH')
    actual_python = {path.relative_to(runtime).as_posix() for path in runtime.rglob('*.py')}
    if actual_python != set(expected_names):
        raise ValueError('RUNTIME_IMPORT_CLOSURE_EXTRA_OR_MISSING_MODULE')
    for name in expected_names:
        item = runtime_manifest['files'][name]
        if (item.get('source_path') != 'tests/manual/' + name
                or item.get('sha256') != shared.sha(ROOT / 'tests/manual' / name)
                or item.get('bytes') != (ROOT / 'tests/manual' / name).stat().st_size
                or shared.sha(runtime / name) != item['sha256']):
            raise ValueError('RUNTIME_IMPORT_CLOSURE_HASH_MISMATCH:' + name)
    external_bank = calibration_dependencies(ROOT)
    if (not external_bank['available']
            or runtime_manifest.get('external_calibration_bank') != external_bank):
        raise ValueError('RUNTIME_EXTERNAL_CALIBRATION_BANK_MISSING_OR_CHANGED')
    return actual, module, runtime_sources


def plain_environment():
    env = os.environ.copy()
    env.update(PYTHONDONTWRITEBYTECODE='1', PYTHONIOENCODING='utf-8')
    env.pop('PYTHONPATH', None)
    return env


def python_tests(label, source_root, pattern):
    return h.run(label, [sys.executable, '-B', '-X', 'utf8', '-m', 'unittest',
                        'discover', '-s', str(source_root / 'tests/manual'), '-p', pattern],
                 'Offline contracts; frozen/current source as specified; no game or OS input.',
                 runtime=RELEASE, env=plain_environment())


def verify(args):
    with shared_transaction():
        actual, module, runtime_sources = check_package(args)
        records = {r['label']: r for r in shared.read_json(TX / 'commands.json')}
        for label in ('BASELINE', 'BASELINE_STORAGE'):
            if records.get(label, {}).get('exit_status') != 0:
                raise ValueError('PRESERVED_BASELINE_RESULT_REQUIRED: ' + label)
        python_tests('BASELINE_SCROLL_TESTS', TX / 'baseline/source', 'test_collection_scroll.py')
        shutil.copy2(shared.confined(RELEASE, args.runtime_module), TX / 'MODIFIED_FILE.py')
        current_hashes = {name: shared.sha(RELEASE / name) for name in sorted(actual)}
        shared.write_json(TX / 'runtime_source_verification.json', runtime_sources)
        h.ui('MODIFIED', RELEASE, TX / 'snapshots')
        h.run('MODIFIED_STORAGE', [str(RELEASE / 'RelinkStudio.exe'), '--storage-self-test'],
              'Updated package; temporary SQLite; no game.', 'STORAGE_SELF_TEST=PASS', runtime=RELEASE)
        for target, marker, extra in [
            ('windows_capture_tests', 'WINDOWS_CAPTURE_TESTS=PASS', []),
            ('windows_ocr_tests', 'WINDOWS_OCR_TESTS=PASS', [str(RELEASE / 'vision/windows_ocr_worker.ps1')]),
            ('workspace_tests', 'WORKSPACE_TESTS=PASS', []),
            ('ui_module_tests', 'UI_MODULE_TESTS=PASS', []),
            ('collection_layout_tests', 'COLLECTION_LAYOUT_TESTS=PASS', []),
        ]:
            env = h.environment(RELEASE)
            env['QT_PLUGIN_PATH'] = str(RELEASE)
            label = 'MODIFIED_LAYOUT_TESTS' if target == 'collection_layout_tests' else target.upper()
            h.run(label, [str(args.build_dir / (target + '.exe')), *extra],
                  'Synthetic/temporary input; packaged dependencies; offscreen; no game input.',
                  marker, runtime=RELEASE, env=env)
        for label, pattern in [('MODIFIED_COLLECTION_TESTS', 'test_collection*.py'),
                               ('MODIFIED_BATCH_TESTS', 'test_foreground*.py'),
                               ('MODIFIED_CURSOR_TESTS', 'test_cursor_motion.py')]:
            python_tests(label, ROOT, pattern)
        h.run('MODIFIED_PACKAGING_TESTS', [sys.executable, '-B', '-X', 'utf8', '-m', 'unittest',
              'discover', '-s', str(ROOT / 'tests/release'), '-p', 'test_collection_runtime_package.py'],
              'Temporary inert packaging fixtures including missing, valid, corrupt and escaping external bank cases; '
              'no actual package modification, model or game input.', runtime=RELEASE, env=plain_environment())
        h.run('PACKAGE_TOOL_TESTS', [sys.executable, '-B', '-X', 'utf8', str(Path(__file__).resolve()), 'self-test'],
              'Verifier unit contracts; temporary inert Python files; helper globals/defaults restore correctly.',
              'COLLECTION_SCROLL_PACKAGE_TOOL_TESTS=PASS', runtime=RELEASE, env=plain_environment())
        # --help exits before session/model creation. -B also protects package
        # hashes from import-generated __pycache__ files.
        h.run('MODIFIED_CLI_HELP', [sys.executable, '-B', '-X', 'utf8',
                                  str(RELEASE / args.runtime_entry), '--help'],
              'Delivered CLI --help only; argument parsing, no session or capture.',
              '--scroll-profile', runtime=RELEASE, env=plain_environment())
        runtime_dir = (RELEASE / args.runtime_module).parent
        import_probe = (
            'import pathlib,sys; sys.path.insert(0,sys.argv[1]); '
            'import collection_scroll,run_collection_trial,collection_live_session; '
            'assert pathlib.Path(collection_scroll.__file__).resolve().parent == pathlib.Path(sys.argv[1]).resolve(); '
            'assert pathlib.Path(run_collection_trial.__file__).resolve().parent == pathlib.Path(sys.argv[1]).resolve(); '
            'assert run_collection_trial.ROOT == pathlib.Path(sys.argv[2]).resolve(); '
            'assert collection_live_session.ROOT == pathlib.Path(sys.argv[2]).resolve(); '
            'assert "navigate_lobby_to_warehouse" not in sys.modules; '
            'print("COLLECTION_PACKAGED_RUNTIME_IMPORTS=PASS")')
        h.run('MODIFIED_RUNTIME_IMPORTS', [sys.executable, '-B', '-X', 'utf8', '-c', import_probe, str(runtime_dir), str(ROOT)],
              'Import delivered pure modules; verify delivered origin and project ROOT; Win32 navigation module remains unimported.',
              'COLLECTION_PACKAGED_RUNTIME_IMPORTS=PASS', runtime=RELEASE, env=plain_environment())
        resources = h.run('MODIFIED_RUNTIME_RESOURCES', [sys.executable, '-B', '-X', 'utf8',
                          str(RELEASE / args.runtime_entry), '--verify-runtime-only'],
              'Resolve published entry root/executable/Python/OCR helper/model files and verify model hashes only; '
              'no model initialization, capture, or Win32 input.',
              '"status": "passed"', runtime=RELEASE, env=plain_environment())
        runtime_check = json.loads(resources['stdout'])
        if (runtime_check.get('project_root') != str(ROOT)
                or runtime_check.get('resources', {}).get('executable', {}).get('path') != str(RELEASE / 'RelinkStudio.exe')
                or runtime_check['resources']['executable'].get('sha256') != shared.sha(RELEASE / 'RelinkStudio.exe')
                or runtime_check.get('game_input_sent') is not False
                or runtime_check.get('game_capture_performed') is not False
                or runtime_check.get('model_initialized') is not False
                or runtime_check.get('models_copied') is not False):
            raise ValueError('PUBLISHED_RUNTIME_RESOURCES_OR_ROOT_INVALID')
        shared.write_json(TX / 'runtime_resource_verification.json', runtime_check)
        bank_probe = (
            'import json,pathlib,sys; sys.path.insert(0,sys.argv[1]); import run_collection_trial as runner; '
            'assert runner.ROOT == pathlib.Path(sys.argv[2]).resolve(); '
            'profile,bank,source=runner.select_scroll_configuration(); '
            'assert profile is None and bank and source["selection"] == "default_bank"; '
            'assert source["profile_hashes_verified"] and source["evidence_hashes_verified"] '
            'and source["evidence_semantics_verified"]; '
            'assert "navigate_lobby_to_warehouse" not in sys.modules; '
            'print(json.dumps(dict(schema="collection-published-default-bank-check-v1",status="passed",'
            'source=source,deltas=[p["delta"] for p in bank],game_capture_performed=False,game_input_sent=False)))')
        bank_record = h.run('MODIFIED_RUNTIME_BANK', [sys.executable, '-B', '-X', 'utf8', '-c', bank_probe,
                             str(runtime_dir), str(ROOT)],
              'Published runner selects project-bound default calibration bank; verifies every profile/evidence hash '
              'and measurement semantics without session, model initialization, capture or Win32 input.',
              '"status": "passed"', runtime=RELEASE, env=plain_environment())
        bank_check = json.loads(bank_record['stdout'])
        if (bank_check.get('status') != 'passed' or bank_check.get('source', {}).get('selection') != 'default_bank'
                or bank_check.get('game_input_sent') is not False
                or bank_check.get('game_capture_performed') is not False):
            raise ValueError('PUBLISHED_DEFAULT_BANK_VERIFICATION_FAILED')
        shared.write_json(TX / 'runtime_bank_verification.json', dict(
            published_loader=bank_check, external_dependencies=calibration_dependencies(ROOT)))
        rollback = TX / ('rollback_test' if not (TX / 'rollback_test').exists()
                         else 'rollback_test_' + uuid.uuid4().hex[:10])
        shutil.copytree(RELEASE, rollback)
        sentinel = rollback / 'collection/user-local-settings.json'
        sentinel.parent.mkdir(parents=True, exist_ok=True)
        sentinel.write_text('{"preserve":true}\n', encoding='utf-8')
        database = rollback / 'preserve-new-ledger.sqlite'
        with sqlite3.connect(database) as connection:
            connection.execute('CREATE TABLE sentinel (value TEXT NOT NULL)')
            connection.execute('INSERT INTO sentinel VALUES (?)', ('preserve new ledger',))
        saved = {str(path): shared.sha(path) for path in (sentinel, database)}
        shared.write_json(TX / 'rollback_target.json', dict(path=str(rollback)))
        shared.prepare_rollback(current_hashes)
        bash = Path(r'C:\Program Files\Git\bin\bash.exe')
        script = (TX / 'ROLLBACK.sh').relative_to(ROOT).as_posix()
        h.run('ROLLBACK', [str(bash), '-c', f'chmod +x "{script}" && "./{script}" "$1"', 'rollback', str(rollback)],
              'Isolated modified package copy; restore baseline files and remove newly managed runtime scripts; '
              'retain unmanaged runtime JSON and SQLite sentinels; never touch game favorites.',
              'ROLLBACK_RESTORED=PASS', runtime=bash.parent)
        original = shared.read_json(TX / 'baseline_release_hashes.json')
        for name, digest in original.items():
            if shared.sha(shared.confined(rollback, name)) != digest:
                raise ValueError('RESTORED_PACKAGE_HASH_MISMATCH: ' + name)
        if any((rollback / name).exists() for name in current_hashes if name not in original):
            raise ValueError('ADDED_MANAGED_RUNTIME_FILE_REMAINS')
        if any(shared.sha(Path(path)) != digest for path, digest in saved.items()):
            raise ValueError('USER_SENTINEL_CHANGED')
        h.ui('RESTORED', rollback, TX / 'restored_snapshots')
        h.run('RESTORED_STORAGE', [str(rollback / 'RelinkStudio.exe'), '--storage-self-test'],
              'Restored baseline on isolated copy; temporary SQLite; no game.',
              'STORAGE_SELF_TEST=PASS', runtime=rollback)
        # Native rollback returns to the pre-CLI package. Separately rerun the
        # exact frozen source scroll contract to record its original behavior.
        python_tests('RESTORED_SCROLL_TESTS', TX / 'baseline/source', 'test_collection_scroll.py')
        frozen_sources()
        for name, digest in current_hashes.items():
            if shared.sha(shared.confined(RELEASE, name)) != digest:
                raise ValueError('MODIFIED_PACKAGE_NOT_RETAINED: ' + name)
        shared.write_json(TX / 'package_verification.json', dict(
            package_verified=True, rollback_target=str(rollback),
            baseline_exe_sha256=shared.sha(BASELINE / 'RelinkStudio.exe'),
            modified_exe_sha256=shared.sha(RELEASE / 'RelinkStudio.exe'),
            native_exe_changed=shared.sha(BASELINE / 'RelinkStudio.exe') != shared.sha(RELEASE / 'RelinkStudio.exe'),
            modified_module=module, runtime_entry=args.runtime_entry, sentinels=saved,
            baseline_read_only=True, game_actions=0, build_invoked=False,
            restored_runtime_status='baseline package restored; added managed CLI scripts removed',
            live_business_asserted=False))
        print('COLLECTION_SCROLL_PACKAGE=PASS; python_module_changed=true; '
              'isolated_rollback=true; baseline_read_only=true; live_business_not_asserted=true')


def finalize(args):
    with shared_transaction():
        actual, module, runtime_sources = check_package(args)
        records = shared.read_json(TX / 'commands.json')
        latest = {record['label']: record for record in records}
        for label in REQUIRED:
            if latest.get(label, {}).get('exit_status') != 0:
                raise ValueError('REQUIRED_VERIFICATION_NOT_PASSED: ' + label)
        package = shared.read_json(TX / 'package_verification.json')
        if not package.get('package_verified') or package.get('modified_module') != module:
            raise ValueError('PACKAGE_OR_PRIMARY_MODULE_CHANGED_SINCE_VERIFY')
        if shared.read_json(TX / 'runtime_source_verification.json') != runtime_sources:
            raise ValueError('RUNTIME_SOURCES_CHANGED_SINCE_VERIFY')
        runtime_check = shared.read_json(TX / 'runtime_resource_verification.json')
        if runtime_check.get('project_root') != str(ROOT):
            raise ValueError('RUNTIME_RESOLVED_ROOT_CHANGED')
        for item in (*runtime_check['resources'].values(), *runtime_check['models'].values()):
            if shared.sha(Path(item['path'])) != item['sha256']:
                raise ValueError('RUNTIME_RESOURCE_CHANGED_SINCE_VERIFY:' + item['path'])
        bank_check = shared.read_json(TX / 'runtime_bank_verification.json')
        if bank_check.get('external_dependencies') != calibration_dependencies(ROOT):
            raise ValueError('EXTERNAL_CALIBRATION_BANK_CHANGED_SINCE_VERIFY')
        if bank_check.get('published_loader', {}).get('source', {}).get('selection') != 'default_bank':
            raise ValueError('PUBLISHED_DEFAULT_BANK_NOT_VERIFIED')
        hashes = shared.read_json(TX / 'modified_release_hashes.json')
        if actual != set(hashes):
            raise ValueError('PACKAGE_FILE_SET_CHANGED_SINCE_VERIFY')
        for name, digest in hashes.items():
            if shared.sha(shared.confined(RELEASE, name)) != digest:
                raise ValueError('PACKAGE_DEPENDENCY_CHANGED_SINCE_VERIFY: ' + name)
        if shared.sha(TX / 'MODIFIED_FILE.py') != module['modified_sha256']:
            raise ValueError('MODIFIED_FILE_COPY_CHANGED')
        rollback = Path(package['rollback_target'])
        for name, digest in shared.read_json(TX / 'baseline_release_hashes.json').items():
            if shared.sha(shared.confined(rollback, name)) != digest:
                raise ValueError('ROLLBACK_PACKAGE_CHANGED: ' + name)
        for path, digest in package['sentinels'].items():
            if shared.sha(Path(path)) != digest:
                raise ValueError('ROLLBACK_USER_SENTINEL_CHANGED')
        diff, changes = shared.source_diff(ROOT, TX)
        if not diff or not any(x['path'] == args.source_module for x in changes):
            raise ValueError('PRIMARY_SOURCE_DIFF_MISSING')
        (TX / 'DIFF_FILE').write_text(diff, encoding='utf-8')
        shared.write_json(TX / 'source_changes.json', changes)
        live = shared.read_json(args.live_result) if args.live_result.is_file() else None
        branch = subprocess.check_output(['git', 'branch', '--show-current'], cwd=ROOT, text=True).strip()
        lines = [
            'Collection scroll and runtime delivery transaction',
            'Recorded at: ' + datetime.now().astimezone().isoformat(),
            'Changed branch: ' + (branch or '(detached)'),
            'Changed fields: ' + args.changed_fields,
            'Baseline comparison: exact frozen working tree, not git HEAD.',
            'BASELINE_EXE_SHA256=' + package['baseline_exe_sha256'],
            'MODIFIED_EXE_SHA256=' + package['modified_exe_sha256'],
            'NATIVE_EXE_CHANGED=' + str(package['native_exe_changed']).lower(),
            'BASELINE_PYTHON_SHA256=' + module['baseline_sha256'],
            'MODIFIED_PYTHON_SHA256=' + module['modified_sha256'],
            'DELIVERED_PYTHON=' + str(RELEASE / args.runtime_module),
            'DELIVERY=' + str(RELEASE / 'RelinkStudio.exe'),
            'LIVE_CLI_ENTRY=' + str(RELEASE / args.runtime_entry),
            'PUBLISHED_DEFAULT_BANK=' + bank_check['published_loader']['source']['path'],
            'EXTERNAL_CALIBRATION_FILES=' + str(len(bank_check['external_dependencies']['files'])),
            'Runtime delivery is project-bound: model/Python/calibration resources keep their recorded project paths; '
            'it is not a self-contained portable CLI package.',
            'Restored behavior/status: isolated original package hashes, offscreen UI and SQLite passed; '
            'new managed runtime files removed; unmanaged user data retained; frozen scroll contracts passed.',
            'Rollback restores program files only, not existing game favorites or external user configuration.',
            'Fixed unpacked release remains MODIFIED; unchanged native EXE does not negate changed delivered Python.',
            'No visible frontend launched by this verifier; native UI Run integration is not asserted.',
            'Live collection completion and speed improvements are not inferred from offline package tests.',
            'LIVE_RESULTS=' + (str(args.live_result) if live is not None else '(not supplied)'),
        ]
        lines += [name + '=' + str(TX / name) for name in ARTIFACTS]
        if live is not None:
            lines += ['', 'LIVE_RESULT_LITERAL_JSON:', json.dumps(live, ensure_ascii=False, indent=2)]
        lines += ['', 'EXACT_COMMANDS_INPUTS_LITERAL_OUTPUT_AND_EXIT_STATUS:']
        for record in records:
            lines.extend(['', record['label'], 'COMMAND: ' + record['command'],
                          'INPUT: ' + record['input'], 'STDOUT:', record.get('stdout', '') or '(empty)',
                          'STDERR:', record.get('stderr', '') or '(empty)',
                          'EXIT_STATUS: ' + str(record['exit_status'])])
        lines += ['', 'PROGRAM_ROLLBACK_TEST=PASS', 'DELIVERY_REMAINS_MODIFIED=true']
        (TX / 'VERIFICATION.txt').write_text('\n'.join(lines) + '\n', encoding='utf-8')
        reopened = {}
        for name in ARTIFACTS:
            path = TX / name
            data = path.read_bytes()
            if not data:
                raise ValueError('EMPTY_TRANSACTION_ARTIFACT: ' + name)
            reopened[name] = dict(path=str(path), bytes=len(data), sha256=shared.sha(path))
        shared.write_json(TX / 'reopened_artifacts.json', reopened)
        print('COLLECTION_SCROLL_DELIVERY=PASS; python_module_changed=true; rollback=PASS; release_remains_modified=true')


def self_test(_args):
    class ToolTests(unittest.TestCase):
        def test_shared_globals_restore_without_modifying_definition_defaults(self):
            before = [(m, m.TX, m.BASELINE, m.RELEASE) for m in (shared, h)]
            defaults = shared.check_frozen.__defaults__, shared.source_diff.__defaults__
            with shared_transaction():
                self.assertEqual(shared.TX, TX)
                self.assertEqual(h.BASELINE, BASELINE)
            for module, tx, baseline, release in before:
                self.assertEqual((module.TX, module.BASELINE, module.RELEASE), (tx, baseline, release))
            self.assertEqual((shared.check_frozen.__defaults__, shared.source_diff.__defaults__), defaults)

        def test_changed_python_is_required_even_if_native_exe_is_unchanged(self):
            with tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                tx, release = root / 'tx', root / 'release'
                source = 'tests/manual/collection_scroll.py'
                runtime = 'collection/collection_scroll.py'
                for folder in (tx / 'baseline/source/tests/manual', root / 'tests/manual', release / 'collection'):
                    folder.mkdir(parents=True)
                original = tx / 'baseline/source' / source
                original.write_text('old = True\n', encoding='utf-8')
                shared.write_json(tx / 'baseline_source_hashes.json', {source: shared.sha(original)})
                (root / source).write_text('new = True\n', encoding='utf-8')
                shutil.copy2(root / source, release / runtime)
                result = assert_changed_module(root, tx, release, source, runtime)
                self.assertNotEqual(result['baseline_sha256'], result['modified_sha256'])
                (release / runtime).write_text('wrong = True\n', encoding='utf-8')
                with self.assertRaisesRegex(ValueError, 'DELIVERED_PYTHON_MODULE_DIFFERS'):
                    assert_changed_module(root, tx, release, source, runtime)
                shutil.copy2(original, root / source)
                shutil.copy2(original, release / runtime)
                with self.assertRaisesRegex(ValueError, 'PRIMARY_PYTHON_MODULE_UNCHANGED'):
                    assert_changed_module(root, tx, release, source, runtime)
                original.write_text('tampered = True\n', encoding='utf-8')
                with self.assertRaisesRegex(ValueError, 'PRIMARY_MODULE_BASELINE_HASH_MISMATCH'):
                    assert_changed_module(root, tx, release, source, runtime)

        def test_path_escape_rejected(self):
            with tempfile.TemporaryDirectory() as directory:
                with self.assertRaisesRegex(ValueError, 'PATH_OUTSIDE_RECORDED_ROOT'):
                    shared.confined(Path(directory), '../other.py')

    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(ToolTests))
    print('COLLECTION_SCROLL_PACKAGE_TOOL_TESTS=' + ('PASS' if result.wasSuccessful() else 'FAIL')
          + '; tests=' + str(result.testsRun) + '; build_invoked=false; game_actions=0')
    return 0 if result.wasSuccessful() else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('phase', choices=('self-test', 'verify', 'finalize'))
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build_relocated')
    parser.add_argument('--source-module', default='tests/manual/collection_scroll.py')
    parser.add_argument('--runtime-module', default='collection/collection_scroll.py')
    parser.add_argument('--runtime-entry', default='collection/run_collection_observed.py')
    parser.add_argument('--live-result', type=Path, default=TX / 'live_result.json')
    parser.add_argument('--changed-fields', default=(
        'listing_scroll_policy; bounded batch-wheel calibration; processed-window coverage; '
        'fresh-frame rebind; packaged collection CLI source copies'))
    args = parser.parse_args()
    args.build_dir, args.live_result = args.build_dir.resolve(), args.live_result.resolve()
    return {'self-test': self_test, 'verify': verify, 'finalize': finalize}[args.phase](args) or 0


if __name__ == '__main__':
    raise SystemExit(main())
