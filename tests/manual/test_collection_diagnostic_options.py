"""Offline diagnostic option contract; invalid requests stop before window binding."""
import json
import os
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]


class DiagnosticOptions(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.exe = ROOT / 'dist/RelinkStudio/RelinkStudio.exe'
        if os.name != 'nt' or not cls.exe.is_file():
            raise unittest.SkipTest('Windows packaged diagnostic required')

    def rejected(self, *args):
        env = os.environ.copy()
        env['PATH'] = str(self.exe.parent) + os.pathsep + env.get('PATH', '')
        process = subprocess.run([str(self.exe), '--live-capture-check', *args],
            capture_output=True, timeout=10, env=env, creationflags=subprocess.CREATE_NO_WINDOW)
        self.assertEqual(process.returncode, 2)
        data = json.loads(process.stdout)
        self.assertEqual(data.get('error'), 'E_DIAGNOSTIC_ARGUMENTS')
        self.assertFalse(data['capture_passed'])
        self.assertFalse(data['game_input_sent'])
        self.assertEqual(data['image_file_writes'], 0)
        self.assertEqual(data['frames'], [])
        self.assertNotIn('collection_price_image', data)
        self.assertNotIn('collection_catalog_image', data)

    def test_numeric_without_dynamic_layout_is_rejected(self):
        self.rejected('--collection-numeric-price', '--ocr')

    def test_gpu_resource_reuse_requires_server_owner(self):
        self.rejected('--reuse-capture-resources','--focus-policy','caller-owned','--ocr',
            '--target-hwnd','1','--target-pid','1','--return-hwnd','2','--return-pid','2')

    def test_selection_preflight_requires_server_owner(self):
        self.rejected('--collection-selection-preflight','--collection-layout','--collection-observation',
            '--focus-policy','caller-owned','--expected-page','skin_listings','--ocr',
            '--target-hwnd','1','--target-pid','1','--return-hwnd','2','--return-pid','2')

    def test_image_without_dynamic_layout_is_rejected(self):
        self.rejected('--collection-price-image', '--ocr')

    def test_title_image_without_collection_observation_is_rejected(self):
        self.rejected('--collection-title-image', '--ocr')

    def test_local_text_requires_both_real_image_outputs_and_observation(self):
        common=['--ocr','--target-hwnd','1','--target-pid','1','--return-hwnd','2','--return-pid','2']
        for missing in ('--collection-observation','--collection-title-image','--collection-catalog-image'):
            options=['--collection-local-text','--collection-observation','--collection-title-image','--collection-catalog-image']
            options.remove(missing)
            with self.subTest(missing=missing):self.rejected(*common,*options)

    def test_catalog_image_without_collection_observation_is_rejected(self):
        # Supply syntactically valid distinct identities so dependency checks,
        # not missing handle arguments, cause rejection before window binding.
        self.rejected('--collection-catalog-image', '--ocr',
                      '--target-hwnd', '1', '--target-pid', '1',
                      '--return-hwnd', '2', '--return-pid', '2')

    def test_catalog_image_without_ocr_is_rejected(self):
        self.rejected('--collection-catalog-image', '--collection-observation',
                      '--target-hwnd', '1', '--target-pid', '1',
                      '--return-hwnd', '2', '--return-pid', '2')

    def test_empty_reference_is_rejected(self):
        self.rejected('--ocr', '--collection-observation', '--collection-layout',
                      '--collection-receipt-reference', '{}')

    def test_malformed_reference_is_rejected(self):
        self.rejected('--ocr', '--collection-observation', '--collection-layout',
                      '--collection-receipt-reference', '{not json}')

    def test_oversized_reference_is_rejected(self):
        self.rejected('--ocr', '--collection-observation', '--collection-layout',
                      '--collection-receipt-reference', json.dumps({'hint':'x'*4100}))


if __name__ == '__main__':
    unittest.main()
