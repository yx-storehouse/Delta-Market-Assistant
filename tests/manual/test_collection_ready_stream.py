"""Stream selection tests use a fake native reply and never access the desktop."""
import json
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch
from collection_live_session import WindowsBackend
from run_collection_observed import MemoryReviewBackend


class ReadyStreamTests(unittest.TestCase):
    def create(self, **options):
        def init(backend, root, *, persistent_capture=False):
            backend._metadata = {}
        with patch.object(WindowsBackend, '__init__', init):
            return MemoryReviewBackend(Path('fixture'), **options)

    def test_requires_persistent_resource_owner(self):
        for fast in (False, True):
            for reuse in (False, True):
                for ready in (False, True):
                    b = self.create(fast_capture=fast, reuse_capture=reuse, ready_stream=ready)
                    self.assertEqual(b.ready_stream, fast and reuse and ready)
                    self.assertEqual(b._metadata['latest_frame_stream_enabled'], b.ready_stream)

    def test_boolean_is_strict(self):
        for value in (0, 1, None, 'true', [], {}):
            with self.assertRaisesRegex(ValueError, 'BOOLEAN_OPTION'):
                self.create(ready_stream=value)

    def test_request_is_explicit_and_existing_packet_not_relabelled(self):
        b = self.create(fast_capture=True)
        cmd = ['app', '--live-capture-check', '--ocr']
        packet = dict(capture_passed=False, error='FIXTURE_STOP', image_file_writes=0,
            capture_stream=dict(pending_slot_capacity=1), collection_ready_watch=dict(samples=[]))
        reply = subprocess.CompletedProcess(cmd, 1, json.dumps(packet).encode(), b'')
        with patch.object(WindowsBackend, 'capture', return_value=reply) as capture:
            result = b.capture(cmd, 1)
        sent = capture.call_args.args[0]
        self.assertEqual(sent.count('--collection-ready-stream'), 1)
        self.assertIn('--reuse-capture-resources', sent)
        self.assertNotIn('--collection-ready-stream', cmd)
        actual = json.loads(result.stdout)
        for key, value in packet.items():
            self.assertEqual(actual[key], value)

    def test_off_switch_preserves_serial_capture(self):
        for options in (dict(ready_stream=False), dict(fast_capture=False), dict(reuse_capture=False)):
            settings = dict(fast_capture=True)
            settings.update(options)
            b = self.create(**settings)
            reply = subprocess.CompletedProcess([], 1, b'{}', b'')
            with patch.object(WindowsBackend, 'capture', return_value=reply) as capture:
                b.capture(['app', '--live-capture-check'], 1)
            self.assertNotIn('--collection-ready-stream', capture.call_args.args[0])

    def test_mutable_reuse_switch_disables_stream_for_capture_ab(self):
        b = self.create(fast_capture=True)
        b.reuse_capture = False
        with patch.object(WindowsBackend, 'capture', return_value=subprocess.CompletedProcess([], 1, b'{}', b'')) as capture:
            b.capture(['app', '--live-capture-check'], 1)
        self.assertNotIn('--collection-ready-stream', capture.call_args.args[0])


if __name__ == '__main__':
    unittest.main()
