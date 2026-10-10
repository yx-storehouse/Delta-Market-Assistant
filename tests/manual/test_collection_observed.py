import json
import subprocess
import unittest
from unittest.mock import patch

from collection_live_session import WindowsBackend
from run_collection_observed import MemoryReviewBackend


class MemoryReviewCaptureTests(unittest.TestCase):
    def run_capture(self, command, preview=False, local_title=None, lean_text=True, reuse_capture=False):
        backend = object.__new__(MemoryReviewBackend)
        backend.memory_preview=preview
        backend.local_title=local_title
        backend.lean_text=lean_text
        backend.reuse_capture=reuse_capture
        packet = dict(capture_passed=False, error='E_WINDOW_OCCLUDED', image_file_writes=0)
        reply = subprocess.CompletedProcess(command, 1, json.dumps(packet).encode(), b'')
        with patch.object(WindowsBackend, 'capture', return_value=reply) as capture:
            result = backend.capture(command, 10)
        return result, capture.call_args.args[0]

    def test_normal_collection_does_not_encode_full_screen_preview(self):
        result, command = self.run_capture(['app', '--live-capture-check', '--ocr'])
        self.assertNotIn('--preview-stdout', command)
        self.assertNotIn('--collection-price-image', command)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(json.loads(result.stdout)['error'], 'E_WINDOW_OCCLUDED')
        self.assertEqual(json.loads(result.stdout)['diagnostic_actual_command'], command)

    def test_resource_reuse_option_is_explicit_and_not_duplicated(self):
        for value in (False,True):
            _, sent=self.run_capture(['app','--ocr'],reuse_capture=value)
            self.assertEqual('--reuse-capture-resources' in sent,value)
        command=['app','--reuse-capture-resources']
        _,sent=self.run_capture(command,reuse_capture=True)
        self.assertEqual(sent,command)

    def test_explicit_visual_review_keeps_preview_available(self):
        _,command=self.run_capture(['app','--ocr'],preview=True)
        self.assertIn('--preview-stdout',command)

    def test_native_text_is_omitted_only_for_explicit_fixed_local_provider(self):
        for local,lean,expected in ((None,True,False),(object(),True,True),(object(),False,False)):
            _,command=self.run_capture(['app','--collection-observation'],local_title=local,lean_text=lean)
            self.assertEqual('--collection-local-text' in command,expected)
            self.assertIn('--collection-title-image',command)
            self.assertIn('--collection-catalog-image',command)

    def test_price_image_requires_dynamic_layout(self):
        _, command = self.run_capture(['app', '--collection-layout'])
        self.assertIn('--collection-price-image', command)

    def test_title_image_requires_collection_observation(self):
        _, command = self.run_capture(['app', '--collection-observation'])
        self.assertIn('--collection-title-image', command)
        self.assertNotIn('--collection-price-image', command)

    def test_flags_not_duplicated_and_caller_list_unchanged(self):
        command = ['app', '--collection-layout', '--preview-stdout', '--collection-price-image']
        before = list(command)
        _, sent = self.run_capture(command)
        self.assertEqual(command, before)
        self.assertEqual(sent, before)


if __name__ == '__main__':
    unittest.main()
