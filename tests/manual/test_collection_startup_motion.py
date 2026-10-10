"""Startup ordering and failure cleanup; injected clock/backend, no OS input."""
import ast
import inspect
from pathlib import Path
import tempfile
import textwrap
import unittest

from collection_live_session import ForegroundSession, WindowsBackend
from cursor_motion import MotionInterrupted
from test_collection_live_session import Clock, FakeBackend, capture


class StartupMotionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.clock = Clock()
        self.backend = FakeBackend(self.clock)
        self.timeline = []
        self.started = self.clock.now()
        original_enter = self.backend.enter
        original_neutral = self.backend.neutral_pointer
        def enter():
            self.timeline.append(('activate', self.clock.now()))
            original_enter()
        def neutral():
            self.timeline.append(('neutral', self.clock.now()))
            original_neutral()
        self.backend.enter = enter
        self.backend.neutral_pointer = neutral

    def wait(self, seconds):
        self.timeline.append(('wait', seconds))
        self.clock.wait(seconds)

    def session(self, **options):
        return ForegroundSession('artifacts/startup/session.json', root=self.root,
            backend=self.backend, now=self.clock.now, wait=self.wait, **options)

    def test_settle_finishes_before_first_pointer_motion(self):
        session = self.session(entry_settle_seconds=.9)
        with session:
            self.assertEqual([event[0] for event in self.timeline], ['activate', 'wait', 'neutral'])
            self.assertAlmostEqual(self.timeline[2][1] - self.started, .9)
            self.assertEqual(session.report['entry_settle_ms'], 900)
            session.perform(capture())
        self.assertEqual(self.backend.events.count('neutral'), 1)
        self.assertEqual(self.backend.events.count('enter'), 1)
        self.assertEqual(self.backend.events.count('leave'), 1)
        self.assertTrue(session.report['ide_restored'])

    def test_windows_activation_method_contains_no_pointer_motion(self):
        tree = ast.parse(textwrap.dedent(inspect.getsource(WindowsBackend.enter)))
        calls = [node.func.attr for node in ast.walk(tree)
                 if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)]
        self.assertIn('activate', calls)
        self.assertNotIn('neutral_pointer', calls)
        self.assertNotIn('_position', calls)
        self.assertNotIn('_move_screen', calls)
        self.assertNotIn('SetCursorPos', calls)

    def test_foreground_loss_during_settle_stops_before_pointer_motion(self):
        def lose_focus(seconds):
            self.wait(seconds)
            self.backend.active = False
        session = ForegroundSession('artifacts/startup/session.json', root=self.root,
            backend=self.backend, now=self.clock.now, wait=lose_focus)
        with self.assertRaisesRegex(RuntimeError, 'BATCH_FOREGROUND_LOST'):
            session.enter()
        self.assertNotIn('neutral', self.backend.events)
        self.assertEqual(self.backend.events.count('enter'), 1)
        self.assertEqual(self.backend.events.count('leave'), 1)
        self.assertEqual(session.steps, [])
        self.assertTrue(session.report['ide_restored'])

    def test_invalid_viewport_after_settle_stops_before_pointer_motion(self):
        self.backend.viewport_size = (0, 1440)
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'COLLECTION_ENTRY_VIEWPORT'):
            session.enter()
        self.assertEqual(session.report['entry_settle_ms'], 900)
        self.assertNotIn('neutral', self.backend.events)
        self.assertEqual(self.backend.events.count('leave'), 1)

    def test_occluded_neutral_target_stops_and_restores_once(self):
        attempts = []
        def occluded():
            attempts.append(self.clock.now())
            raise RuntimeError('COLLECTION_TARGET_OCCLUDED')
        self.backend.neutral_pointer = occluded
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'COLLECTION_TARGET_OCCLUDED'):
            session.enter()
        self.assertEqual(len(attempts), 1)
        self.assertAlmostEqual(attempts[0] - self.started, .9)
        session.finish()
        self.assertEqual(self.backend.events.count('leave'), 1)
        self.assertEqual(session.steps, [])

    def test_real_interference_is_terminal_and_never_retried(self):
        attempts = []
        def interference():
            attempts.append(self.clock.now())
            metadata = dict(completed=False, elapsed_ms=12.6, error='CURSOR_INTERFERENCE')
            raise MotionInterrupted('CURSOR_INTERFERENCE', metadata)
        self.backend.neutral_pointer = interference
        session = self.session()
        with self.assertRaisesRegex(MotionInterrupted, 'CURSOR_INTERFERENCE'):
            session.enter()
        self.assertEqual(len(attempts), 1)
        self.assertAlmostEqual(attempts[0] - self.started, .9)
        self.assertEqual(session.report['manual_clicks'], 0)
        self.assertEqual(session.steps, [])
        self.assertEqual(self.backend.events.count('enter'), 1)
        self.assertEqual(self.backend.events.count('leave'), 1)
        self.assertTrue(session.report['ide_restored'])

    def test_budget_expiry_before_motion_never_moves_pointer(self):
        original = self.backend.enter
        def delayed_activation():
            original()
            self.clock.wait(1.1)
        self.backend.enter = delayed_activation
        session = self.session(timeout_seconds=1)
        with self.assertRaisesRegex(RuntimeError, 'BATCH_DEADLINE'):
            session.enter()
        self.assertNotIn('neutral', self.backend.events)
        self.assertEqual(self.backend.events.count('leave'), 1)

    def test_neutral_delegate_keeps_target_ownership_and_endpoint_guard_path(self):
        # Bypass constructor so this test never loads Win32. The actual neutral
        # method must delegate to _position rather than a raw cursor setter.
        backend = WindowsBackend.__new__(WindowsBackend)
        backend.viewport = lambda: (2560, 1440)
        calls = []
        backend._position = lambda point, purpose: calls.append((point, purpose))
        backend.neutral_pointer()
        self.assertEqual(calls, [([1280, 480], 'neutral_pointer')])


if __name__ == '__main__':
    unittest.main()
