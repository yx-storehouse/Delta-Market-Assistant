"""Temporary inert source packaging tests, never game/runtime execution."""
import json
from pathlib import Path
import tempfile
import unittest

from package_collection_runtime import DEFAULT_BANK, calibration_dependencies, import_closure, package_runtime, sha


class RuntimePackagingTests(unittest.TestCase):
    def fixture(self, root):
        source = root / 'tests/manual'
        source.mkdir(parents=True)
        (source / 'run_collection_observed.py').write_text(
            'import alpha\ndef deferred():\n    from beta import value\n', encoding='utf-8')
        (source / 'alpha.py').write_text('import json\nimport gamma\n', encoding='utf-8')
        (source / 'beta.py').write_text('from alpha import value\nvalue = 1\n', encoding='utf-8')
        (source / 'gamma.py').write_text('value = 2\n', encoding='utf-8')
        (source / 'unrelated_legacy.py').write_text('raise RuntimeError("must not run")\n', encoding='utf-8')
        (source / 'large.onnx').write_bytes(b'not copied')
        return source

    def test_ast_includes_deferred_imports_and_cycles_but_not_unrelated_or_models(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = self.fixture(root)
            names = import_closure(source)
            self.assertEqual(names, ['alpha.py', 'beta.py', 'gamma.py', 'run_collection_observed.py'])
            manifest = package_runtime(root, root / 'dist/RelinkStudio')
            runtime = root / 'dist/RelinkStudio/collection'
            self.assertEqual(set(p.name for p in runtime.iterdir()), set(names) | {'runtime_manifest.json'})
            self.assertEqual(manifest['model_files_copied'], 0)
            self.assertFalse(manifest['external_calibration_bank']['available'])
            self.assertEqual(manifest['external_calibration_bank']['files'], {})
            for name in names:
                self.assertEqual(sha(source / name), sha(runtime / name))

    def test_removed_managed_module_is_removed_unmanaged_user_data_is_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = self.fixture(root)
            package_runtime(root, root / 'dist/RelinkStudio')
            runtime = root / 'dist/RelinkStudio/collection'
            (runtime / 'user-settings.json').write_text('{"retain":true}', encoding='utf-8')
            (source / 'alpha.py').write_text('import json\n', encoding='utf-8')
            package_runtime(root, root / 'dist/RelinkStudio')
            self.assertFalse((runtime / 'gamma.py').exists())
            self.assertEqual(json.loads((runtime / 'user-settings.json').read_text()), {'retain': True})

    def test_changed_obsolete_module_is_preserved_and_stops_before_copy(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = self.fixture(root)
            package_runtime(root, root / 'dist/RelinkStudio')
            runtime = root / 'dist/RelinkStudio/collection'
            old_alpha = (runtime / 'alpha.py').read_bytes()
            (source / 'alpha.py').write_text('import json\n', encoding='utf-8')
            (runtime / 'gamma.py').write_text('user_modified = True\n', encoding='utf-8')
            with self.assertRaisesRegex(ValueError, 'STALE_FILE_CHANGED'):
                package_runtime(root, root / 'dist/RelinkStudio')
            self.assertEqual((runtime / 'alpha.py').read_bytes(), old_alpha)
            self.assertTrue((runtime / 'gamma.py').exists())

    def test_unrecorded_destination_and_path_in_manifest_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.fixture(root)
            with self.assertRaisesRegex(ValueError, 'DESTINATION'):
                package_runtime(root, root / 'elsewhere')
            package_runtime(root, root / 'dist/RelinkStudio')
            manifest = root / 'dist/RelinkStudio/collection/runtime_manifest.json'
            value = json.loads(manifest.read_text())
            value['files']['../escape.py'] = {'sha256': '0' * 64}
            manifest.write_text(json.dumps(value), encoding='utf-8')
            with self.assertRaisesRegex(ValueError, 'MANAGED_NAME'):
                package_runtime(root, root / 'dist/RelinkStudio')

    def bank_fixture(self, root):
        directory = root / 'artifacts/collection_scroll_speed'
        directory.mkdir(parents=True)
        measurement = directory / 'measurement.json'
        measurement.write_text(json.dumps(dict(schema='collection-scroll-calibration-measurement-v1',
                                               status='synthetic_not_live')), encoding='utf-8')
        profile = directory / 'profile.json'
        profile.write_text(json.dumps(dict(schema='collection-scroll-calibration-v1',
            source=dict(record_path=measurement.relative_to(root).as_posix(), record_sha256=sha(measurement)))), encoding='utf-8')
        bank = root / DEFAULT_BANK
        bank.write_text(json.dumps(dict(schema='collection-scroll-calibration-bank-v1', profiles=[
            dict(path=profile.relative_to(root).as_posix(), sha256=sha(profile))])), encoding='utf-8')
        return bank, profile, measurement

    def test_external_bank_profiles_and_measurement_are_hashed_not_copied(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.fixture(root)
            bank, profile, measurement = self.bank_fixture(root)
            manifest = package_runtime(root, root / 'dist/RelinkStudio')
            external = manifest['external_calibration_bank']
            self.assertTrue(external['available'])
            self.assertEqual(external['copied_files'], 0)
            self.assertEqual(len(external['files']), 3)
            for path in (bank, profile, measurement):
                self.assertEqual(external['files'][path.relative_to(root).as_posix()]['sha256'], sha(path))
                self.assertFalse((root / 'dist/RelinkStudio' / path.relative_to(root)).exists())

    def test_invalid_present_bank_never_silently_becomes_absent(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = self.fixture(root)
            bank, profile, measurement = self.bank_fixture(root)
            package_runtime(root, root / 'dist/RelinkStudio')
            delivered = root / 'dist/RelinkStudio/collection/alpha.py'
            before = delivered.read_bytes()
            (source / 'alpha.py').write_text('different = True\n', encoding='utf-8')
            measurement.write_text('{}', encoding='utf-8')
            with self.assertRaisesRegex(ValueError, 'BANK_DEPENDENCY_HASH'):
                package_runtime(root, root / 'dist/RelinkStudio')
            self.assertEqual(delivered.read_bytes(), before)
            bank.write_text('{broken', encoding='utf-8')
            with self.assertRaisesRegex(ValueError, 'BANK_DEPENDENCY_JSON'):
                calibration_dependencies(root)

    def test_external_bank_path_traversal_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.fixture(root)
            bank, profile, measurement = self.bank_fixture(root)
            value = json.loads(bank.read_text())
            value['profiles'][0]['path'] = 'artifacts/../outside.json'
            bank.write_text(json.dumps(value), encoding='utf-8')
            with self.assertRaisesRegex(ValueError, 'BANK_DEPENDENCY_PATH'):
                calibration_dependencies(root)


if __name__ == '__main__':
    unittest.main()
