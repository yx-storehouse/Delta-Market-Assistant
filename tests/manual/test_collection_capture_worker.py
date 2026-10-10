"""Real child-process transport tests with an inert temporary JSONL worker."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

from capture_worker_client import CaptureWorkerClient, CaptureWorkerError
from collection_live_session import ForegroundSession, WindowsBackend
import test_collection_live_session as fixtures


WORKER = r'''
import json, os, sys, time
if '--no-read' in sys.argv:
    time.sleep(60)
for line in sys.stdin.buffer:
    request = json.loads(line)
    mode = request['arguments'][0]
    if mode == 'sleep':
        time.sleep(60)
    if mode == 'exit':
        raise SystemExit(7)
    if mode == 'invalid-json':
        print('not-json', flush=True)
        continue
    if mode == 'partial':
        sys.stdout.write('{')
        sys.stdout.flush()
        raise SystemExit(0)
    if mode == 'oversized':
        print('x' * 2048, flush=True)
        continue
    if mode == 'stderr':
        sys.stderr.write('diagnostic\n' * 3000)
        sys.stderr.flush()
    result = {'pid': os.getpid(), 'received': request['arguments'],
              'image_file_writes': 0, 'preview_png_base64': 'eA=='}
    status = 1 if mode == 'status-one' else 0
    envelope = dict(id=request['id'], exit_status=status, result=result)
    if mode == 'wrong-id':
        envelope['id'] += 1
    if mode == 'bool-id':
        envelope['id'] = True
    if mode == 'bool-status':
        envelope['exit_status'] = False
    if mode == 'bad-result':
        envelope['result'] = []
    if mode == 'extra-key':
        envelope['unframed'] = 'unexpected'
    print(json.dumps(envelope), flush=True)
'''


class CaptureWorkerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.script = Path(self.temporary.name) / 'inert_worker.py'
        self.script.write_text(WORKER, encoding='utf-8')
        self.client = CaptureWorkerClient(sys.executable, startup_arguments=(str(self.script),), close_timeout=.2)
        self.addCleanup(self.client.close)

    def request(self, mode='ok', *args, timeout=3):
        return self.client.capture([sys.executable, mode, *args], timeout)

    def assert_poisoned(self):
        metadata = self.client.metadata()
        self.assertTrue(metadata['closed'])
        self.assertTrue(metadata['poisoned'])
        self.assertIsNotNone(self.client.process.poll())
        with self.assertRaisesRegex(CaptureWorkerError, 'WORKER_CLOSED'):
            self.request()
        self.assertEqual(self.client.metadata()['worker_start_count'], 1)

    def test_construction_is_lazy_and_close_without_start_is_idempotent(self):
        self.assertIsNone(self.client.process)
        self.client.close()
        self.client.close()
        self.assertEqual(self.client.metadata()['worker_start_count'], 0)
        with self.assertRaisesRegex(CaptureWorkerError, 'WORKER_CLOSED'):
            self.request()

    def test_two_requests_share_pid_and_preserve_packet_in_memory(self):
        one = self.request('ok', '价格', 'with spaces')
        two = self.request('ok', '--another-argument')
        first, second = json.loads(one.stdout), json.loads(two.stdout)
        self.assertEqual(first['pid'], second['pid'])
        self.assertEqual(first['received'], ['ok', '价格', 'with spaces'])
        self.assertEqual(first['preview_png_base64'], 'eA==')
        self.assertEqual(self.client.metadata()['worker_start_count'], 1)
        self.assertEqual(self.client.metadata()['completed_count'], 2)
        self.assertEqual(list(Path(self.temporary.name).iterdir()), [self.script])
        self.client.close()
        self.assertEqual(self.client.metadata()['exit_status'], 0)

    def test_diagnostic_failure_does_not_replay_or_restart(self):
        reply = self.request('status-one')
        self.assertEqual(reply.returncode, 1)
        self.assertIsNone(self.client.process.poll())
        self.assertEqual(self.client.metadata()['request_count'], 1)
        self.request()
        self.assertEqual(self.client.metadata()['worker_start_count'], 1)

    def test_timeout_kills_worker_and_is_permanent(self):
        started = time.monotonic()
        with self.assertRaisesRegex(CaptureWorkerError, 'WORKER_TIMEOUT$'):
            self.request('sleep', timeout=.15)
        self.assertLess(time.monotonic() - started, 3)
        self.assert_poisoned()

    def test_blocked_pipe_write_obeys_same_deadline(self):
        self.client.startup_arguments += ('--no-read',)
        started = time.monotonic()
        with self.assertRaisesRegex(CaptureWorkerError, 'WORKER_TIMEOUT$'):
            self.request('x' * 60000, timeout=.15)
        self.assertLess(time.monotonic() - started, 3)
        self.assert_poisoned()

    def test_early_exit_is_not_restarted(self):
        with self.assertRaisesRegex(CaptureWorkerError, 'WORKER_EOF'):
            self.request('exit')
        self.assert_poisoned()

    def test_invalid_json_is_terminal_without_payload_in_error(self):
        with self.assertRaisesRegex(CaptureWorkerError, '^COLLECTION_CAPTURE_WORKER_PROTOCOL$'):
            self.request('invalid-json')
        self.assert_poisoned()

    def test_partial_response_is_terminal(self):
        with self.assertRaisesRegex(CaptureWorkerError, 'WORKER_RESPONSE_LIMIT'):
            self.request('partial')
        self.assert_poisoned()

    def test_oversized_response_is_bounded(self):
        self.client.MAX_RESPONSE_BYTES = 1024
        with self.assertRaisesRegex(CaptureWorkerError, 'WORKER_RESPONSE_LIMIT'):
            self.request('oversized')
        self.assert_poisoned()

    def test_envelope_mismatch_is_terminal(self):
        for mode in ('wrong-id', 'bool-id', 'bool-status', 'bad-result', 'extra-key'):
            with self.subTest(mode=mode):
                other = CaptureWorkerClient(sys.executable, startup_arguments=(str(self.script),), close_timeout=.2)
                try:
                    with self.assertRaisesRegex(CaptureWorkerError, 'WORKER_PROTOCOL'):
                        other.capture([sys.executable, mode], 3)
                    self.assertTrue(other.metadata()['poisoned'])
                    self.assertIsNotNone(other.process.poll())
                finally:
                    other.close()

    def test_stderr_is_drained_and_bounded(self):
        reply = self.request('stderr')
        self.assertEqual(reply.returncode, 0)
        self.assertLessEqual(len(reply.stderr), self.client.MAX_STDERR_BYTES)
        self.client.close()

    def test_bad_options_do_not_start_a_process(self):
        for timeout in (0, -1, True, float('nan'), float('inf'), '1'):
            with self.subTest(timeout=timeout), self.assertRaisesRegex(CaptureWorkerError, 'TIMEOUT_OPTION'):
                self.request(timeout=timeout)
        with self.assertRaisesRegex(CaptureWorkerError, 'WORKER_EXECUTABLE'):
            self.client.capture(['other.exe', 'ok'], 3)
        with self.assertRaisesRegex(CaptureWorkerError, 'WORKER_REQUEST_LIMIT'):
            self.request('x' * 70000)
        self.assertIsNone(self.client.process)

    def test_concurrent_capture_is_rejected_without_replay(self):
        self.client._lock.acquire()
        try:
            with self.assertRaisesRegex(CaptureWorkerError, 'CONCURRENT_REQUEST'):
                self.request()
        finally:
            self.client._lock.release()
        self.assertIsNone(self.client.process)

    def test_backend_delegates_without_one_shot_process(self):
        backend = WindowsBackend.__new__(WindowsBackend)
        backend.capture_worker = self.client
        with patch('collection_live_session.subprocess.run') as one_shot:
            result = backend.capture([sys.executable, 'ok'], 3)
        one_shot.assert_not_called()
        self.assertEqual(result.returncode, 0)

    def test_legacy_backend_retains_one_shot_command(self):
        backend = WindowsBackend.__new__(WindowsBackend)
        backend.capture_worker = None
        expected = subprocess.CompletedProcess(['exe'], 0, b'{}', b'')
        with patch('collection_live_session.subprocess.run', return_value=expected) as one_shot:
            result = backend.capture(['exe', '--live-capture-check'], 3)
        self.assertIs(result, expected)
        self.assertEqual(one_shot.call_args.kwargs['timeout'], 3)

    def test_windows_leave_closes_worker_after_single_restore_even_if_restore_raises(self):
        self.request()
        backend = WindowsBackend.__new__(WindowsBackend)
        backend.capture_worker = self.client
        backend.capture_worker_close_error = None
        backend.identities = dict(return_hwnd=3, return_pid=4)
        backend.pointer = backend.watcher = backend.dpi = None
        backend.halt = threading.Event()
        calls = []
        def activate(hwnd, pid):
            calls.append((hwnd, pid))
            raise RuntimeError('offline-restore-error')
        backend.activate = activate
        with self.assertRaisesRegex(RuntimeError, 'offline-restore-error'):
            backend.leave()
        self.assertEqual(calls, [(3, 4)])
        self.assertTrue(self.client.metadata()['closed'])
        self.assertEqual(self.client.process.poll(), 0)
        self.assertTrue(backend.halt.is_set())

    def test_transport_timeout_after_star_keeps_journal_and_restores_once(self):
        root = Path(self.temporary.name)
        path = root / 'artifacts/trial/input/snapshot.json'
        path.parent.mkdir(parents=True)
        path.write_text(json.dumps(fixtures.snapshot()), encoding='utf-8')
        clock = fixtures.Clock()
        backend = fixtures.FakeBackend(clock, replies=[fixtures.packet()])
        original_capture, original_leave = backend.capture, backend.leave
        captured = []
        def capture(command, timeout):
            captured.append(command)
            if len(captured) == 1:
                return original_capture(command, timeout)
            return self.request('sleep', timeout=.15)
        def leave():
            self.client.close()
            return original_leave()
        backend.capture, backend.leave = capture, leave
        session = ForegroundSession('artifacts/trial/session.json', root=root,
            backend=backend, now=clock.now, wait=clock.wait, fast_settle=True)
        with self.assertRaisesRegex(CaptureWorkerError, 'WORKER_TIMEOUT$'):
            with session:
                session.perform(fixtures.capture())
                session.perform(fixtures.collect())
                session.perform(fixtures.capture(expect_collection_added=True))
        document = json.loads(next(session.journal.directory.glob('*.json')).read_text('utf-8'))
        self.assertEqual(document['status'], 'dispatched')
        self.assertIsNotNone(session.pending)
        self.assertEqual(session.report['manual_clicks'], 1)
        self.assertEqual(backend.events.count('leave'), 1)
        self.assertTrue(session.report['ide_restored'])
        session.finish()
        self.assertEqual(backend.events.count('leave'), 1)
        self.assert_poisoned()

    def test_close_failure_does_not_claim_session_passed(self):
        root = Path(self.temporary.name)
        clock = fixtures.Clock()
        backend = fixtures.FakeBackend(clock)
        original_metadata = backend.metadata
        backend.metadata = lambda: dict(original_metadata(), capture_worker_close_error='offline-close-error')
        session = ForegroundSession('artifacts/trial/session.json', root=root,
            backend=backend, now=clock.now, wait=clock.wait)
        with session:
            session.perform(fixtures.capture())
        self.assertTrue(session.report['ide_restored'])
        self.assertFalse(session.report['passed'])
        self.assertEqual(session.report['error'], 'COLLECTION_CAPTURE_WORKER_CLOSE')


if __name__ == '__main__':
    unittest.main()
