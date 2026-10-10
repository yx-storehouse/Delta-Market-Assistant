"""Filesystem-only resolver/runtime contracts; no OS input or real model run."""
import hashlib
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from collection_paths import MARKERS, project_root
import collection_runtime


class ProjectRootTests(unittest.TestCase):
    def root(self, directory):
        root = Path(directory)
        for marker in MARKERS:
            (root / marker).mkdir(parents=True)
        return root

    def test_development_and_published_resolve_same_root_without_cwd(self):
        with tempfile.TemporaryDirectory() as directory:
            root = self.root(directory)
            for name in ('tests/manual/collection_live_session.py',
                         'dist/RelinkStudio/collection/run_collection_trial.py'):
                self.assertEqual(project_root(root / name, environ={}), root.resolve())

    def test_wrong_layout_and_missing_markers_do_not_guess(self):
        with tempfile.TemporaryDirectory() as directory:
            root = self.root(directory)
            with self.assertRaisesRegex(ValueError, 'COLLECTION_PROJECT_ROOT_NOT_FOUND'):
                project_root(root / 'unrelated/helper.py', environ={})
            (root / '.tools/ocr-runtime').rmdir()
            with self.assertRaisesRegex(ValueError, 'COLLECTION_PROJECT_ROOT_NOT_FOUND'):
                project_root(root / 'tests/manual/example.py', environ={})

    def test_explicit_root_requires_absolute_path_and_all_markers(self):
        with tempfile.TemporaryDirectory() as directory:
            root = self.root(directory)
            self.assertEqual(project_root(root / 'anywhere/helper.py',
                             environ={'RELINK_PROJECT_ROOT': str(root)}), root.resolve())
            for invalid in ('', '.', 'relative/project'):
                with self.subTest(invalid=invalid), self.assertRaisesRegex(ValueError, 'ABSOLUTE_REQUIRED'):
                    project_root(root / 'tests/manual/file.py', environ={'RELINK_PROJECT_ROOT': invalid})
            with self.assertRaisesRegex(ValueError, 'MARKERS_MISSING'):
                project_root(root / 'tests/manual/file.py', environ={'RELINK_PROJECT_ROOT': str(root / 'missing')})

    def test_invalid_explicit_root_does_not_silently_fall_back(self):
        with tempfile.TemporaryDirectory() as directory:
            root = self.root(directory)
            with self.assertRaisesRegex(ValueError, 'ABSOLUTE_REQUIRED'):
                project_root(root / 'dist/RelinkStudio/collection/main.py',
                             environ={'RELINK_PROJECT_ROOT': 'relative'})

    def test_runtime_probe_checks_hashes_without_model_or_desktop_initialization(self):
        with tempfile.TemporaryDirectory() as directory:
            root = self.root(directory)
            for relative in ('dist/RelinkStudio/RelinkStudio.exe', '.tools/ocr-runtime/Scripts/python.exe',
                             'dist/RelinkStudio/vision/windows_ocr_worker.ps1', '.tools/ocr-models/fake.onnx'):
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b'inert test fixture; never executed')
            module = root / 'dist/RelinkStudio/collection/run_collection_observed.py'
            expected = hashlib.sha256((root / '.tools/ocr-models/fake.onnx').read_bytes()).hexdigest()
            with patch.dict(os.environ, {'RELINK_PROJECT_ROOT': str(root)}), \
                    patch.object(collection_runtime, 'MODELS', {'fake.onnx': expected}):
                result = collection_runtime.verify_runtime(module, expected_root=root)
                self.assertEqual(result['project_root'], str(root.resolve()))
                self.assertFalse(result['game_capture_performed'])
                self.assertFalse(result['game_input_sent'])
                self.assertFalse(result['model_initialized'])
                self.assertFalse(result['models_copied'])
                (root / '.tools/ocr-models/fake.onnx').write_bytes(b'wrong')
                with self.assertRaisesRegex(ValueError, 'COLLECTION_RUNTIME_MODEL_HASH'):
                    collection_runtime.verify_runtime(module)
                with self.assertRaisesRegex(ValueError, 'COLLECTION_RUNTIME_ROOT_MISMATCH'):
                    collection_runtime.verify_runtime(module, expected_root=root / 'other')


if __name__ == '__main__':
    unittest.main()
