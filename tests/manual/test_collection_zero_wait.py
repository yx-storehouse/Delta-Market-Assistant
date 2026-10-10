"""Independent zero-post-click-wait checks with fake frames and inputs only."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

from collection_live_session import ForegroundSession
from collection_scroll import selection_plan
from test_collection_dynamic import dynamic_packet, rule
import test_collection_live_session as fixtures


class TimedFakeBackend(fixtures.FakeBackend):
    def __init__(self, clock):
        super().__init__(clock)
        self.capture_started = []
        self.click_returned = []
        self.fast_collection_motion = True
        self.fast_clicks = 0

    def capture(self, command, timeout):
        self.capture_started.append(self.clock.now())
        return super().capture(command, timeout)

    def fast_collection_click(self, point, before_dispatch=None):
        self.fast_clicks += 1
        sent = super().click(point, before_dispatch=before_dispatch)
        self.click_returned.append(self.clock.now())
        return sent


class ZeroWaitTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        snapshot = self.root / 'artifacts/trial/input/snapshot.json'
        snapshot.parent.mkdir(parents=True)
        snapshot.write_text(json.dumps(fixtures.snapshot()), encoding='utf-8')
        self.clock = fixtures.Clock()
        self.backend = TimedFakeBackend(self.clock)
        self.session_waits = []

    def wait(self, seconds):
        self.session_waits.append(seconds)
        self.clock.wait(seconds)

    def session(self, name='session', **options):
        return ForegroundSession(f'artifacts/trial/{name}.json', root=self.root,
            backend=self.backend, now=self.clock.now, wait=self.wait,
            fast_settle=True, **options)

    def action_events(self):
        return [event for event in self.backend.events
                if isinstance(event, tuple) and event[0] in ('click', 'scroll')]

    def records(self, session):
        return [json.loads(path.read_text(encoding='utf-8'))
                for path in session.journal.directory.glob('*.json')]

    def assert_restored_once(self, session):
        self.assertEqual(session.report['leave_calls'], 1)
        self.assertTrue(session.report['ide_restored'])

    def test_immediate_reads_preserve_motion_and_never_call_even_zero_settle(self):
        self.backend.motion_seconds = .1
        self.backend.replies = [dynamic_packet(selected=False), dynamic_packet('b'),
                                dynamic_packet('c', gold=True)]
        session = self.session()
        with session:
            session.perform(fixtures.capture(collection_layout=True))
            self.session_waits.clear()  # Entry settling is intentionally unchanged.
            plan = selection_plan(session.previous, rule(), 'observed-row')['steps']
            selection = session.perform(plan[0])
            self.assertEqual(session.previous, {})
            self.assertIsNotNone(session.pending_geometry)
            self.assertIsNone(session.selected_lease)
            session.perform(plan[1])
            self.assertIsNone(session.pending_geometry)
            self.assertEqual(session.selected_lease['frame_id'], 'fake:b')
            favorite = session.perform(fixtures.collect())
            self.assertEqual(session.previous, {})
            self.assertEqual(self.records(session)[0]['status'], 'dispatched')
            self.assertIsNotNone(session.pending)
            session.perform(fixtures.capture(collection_layout=True, expect_collection_added=True))
            self.assertIsNone(session.pending)
        self.assertEqual(self.session_waits, [])
        self.assertEqual(self.backend.fast_clicks, 2)
        for action in (selection, favorite):
            self.assertEqual(action['post_click_fixed_wait_ms'], 0)
            self.assertEqual(action['timings']['settle_wait_ms'], 0)
            self.assertAlmostEqual(action['timings']['motion_ms'], 100)
        self.assertEqual(self.backend.capture_started[1:], self.backend.click_returned)
        self.assertEqual(self.records(session)[0]['status'], 'confirmed')
        self.assertEqual(len(self.action_events()), 2)
        self.assert_restored_once(session)

    def test_selection_ready_pixels_with_old_frame_id_do_not_unlock_a_star(self):
        stale = dynamic_packet('b')
        for name in ('collection_observation', 'collection_layout', 'collection_selected_card'):
            stale[name]['frame_id'] = 'fake:a'
        self.backend.replies = [dynamic_packet(selected=False), stale, dynamic_packet('c')]
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'BATCH_STEP_FAILED'):
            with session:
                session.perform(fixtures.capture(collection_layout=True))
                plan = selection_plan(session.previous, rule(), 'observed-row')['steps']
                session.perform(plan[0])
                session.perform(plan[1])
        self.assertEqual(session.steps[-1]['attempt_count'], 1)
        self.assertIsNotNone(session.pending_geometry)
        self.assertIsNone(session.selected_lease)
        self.assertEqual(self.records(session), [])
        self.assertEqual(len(self.action_events()), 1)
        self.assertEqual(len(self.backend.replies), 1)
        self.assert_restored_once(session)

    def test_white_receipt_polls_once_with_existing_delay_without_reclick(self):
        self.backend.replies = [dynamic_packet(), dynamic_packet('b'),
                                dynamic_packet('c', gold=True)]
        session = self.session()
        with session:
            session.perform(fixtures.capture(collection_layout=True))
            self.session_waits.clear()
            session.perform(fixtures.collect())
            receipt = session.perform(fixtures.capture(collection_layout=True, expect_collection_added=True))
        self.assertEqual(self.session_waits, [.15])
        self.assertEqual(receipt['attempt_count'], 2)
        self.assertFalse(receipt['attempts'][0]['result']['collection_receipt_passed'])
        self.assertTrue(receipt['attempts'][1]['result']['collection_receipt_passed'])
        self.assertEqual(self.backend.capture_started[1], self.backend.click_returned[0])
        self.assertEqual(self.backend.fast_clicks, 1)
        self.assertEqual(self.records(session)[0]['status'], 'confirmed')
        self.assert_restored_once(session)

    def test_receipt_timeout_keeps_dispatched_and_blocks_same_and_new_session_replay(self):
        self.backend.replies = [dynamic_packet(), subprocess.TimeoutExpired('fake_capture', 1)]
        session = self.session()
        with self.assertRaises(subprocess.TimeoutExpired):
            with session:
                session.perform(fixtures.capture(collection_layout=True))
                session.perform(fixtures.collect())
                session.perform(fixtures.capture(collection_layout=True, expect_collection_added=True))
        self.assertEqual(self.records(session)[0]['status'], 'dispatched')
        self.assertIsNotNone(session.pending)
        with self.assertRaisesRegex(RuntimeError, 'SESSION_NOT_ACTIVE'):
            session.perform(fixtures.collect())
        self.assert_restored_once(session)
        restarted = self.session('restarted')
        self.backend.replies = [dynamic_packet('d')]
        with self.assertRaisesRegex(RuntimeError, 'PENDING_RECONCILIATION'):
            with restarted:
                restarted.perform(fixtures.capture(collection_layout=True))
                restarted.perform(fixtures.collect())
        self.assertEqual(self.backend.fast_clicks, 1)
        self.assertEqual(len(self.action_events()), 1)
        self.assertEqual(self.records(restarted)[0]['status'], 'dispatched')
        self.assert_restored_once(restarted)

    def test_partial_send_never_settles_or_captures_and_keeps_uncertain_attempt(self):
        self.backend.sent = 1
        self.backend.replies = [dynamic_packet()]
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'COLLECTION_INPUT_UNCERTAIN'):
            with session:
                session.perform(fixtures.capture(collection_layout=True))
                self.session_waits.clear()
                session.perform(fixtures.collect())
        self.assertEqual(self.session_waits, [])
        self.assertEqual(self.records(session)[0]['status'], 'input_uncertain')
        self.assertIsNotNone(session.pending)
        self.assertEqual(len(self.backend.capture_started), 1)
        self.assertEqual(len(self.action_events()), 1)
        with self.assertRaisesRegex(RuntimeError, 'SESSION_NOT_ACTIVE'):
            session.perform(fixtures.collect())
        self.assert_restored_once(session)

    def test_foreground_loss_immediately_after_send_keeps_dispatch_without_receipt(self):
        original = self.backend.fast_collection_click
        def lose_focus(point, before_dispatch=None):
            sent = original(point, before_dispatch=before_dispatch)
            self.backend.active = False
            return sent
        self.backend.fast_collection_click = lose_focus
        self.backend.replies = [dynamic_packet()]
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'BATCH_FOREGROUND_LOST'):
            with session:
                session.perform(fixtures.capture(collection_layout=True))
                self.session_waits.clear()
                session.perform(fixtures.collect())
        self.assertEqual(self.session_waits, [])
        self.assertEqual(self.records(session)[0]['status'], 'dispatched')
        self.assertIsNotNone(session.pending)
        self.assertEqual(len(self.backend.capture_started), 1)
        self.assertEqual(len(self.action_events()), 1)
        self.assert_restored_once(session)

    def test_motion_expiry_still_stops_before_dispatch_despite_zero_post_wait(self):
        self.backend.motion_seconds = 5.1
        self.backend.replies = [dynamic_packet()]
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'OBSERVATION_EXPIRED'):
            with session:
                session.perform(fixtures.capture(collection_layout=True))
                self.session_waits.clear()
                session.perform(fixtures.collect())
        self.assertEqual(self.session_waits, [])
        self.assertEqual(self.action_events(), [])
        self.assertEqual(self.records(session)[0]['status'], 'prepared')
        self.assertIsNotNone(session.pending)
        self.assert_restored_once(session)


if __name__ == '__main__':
    unittest.main()
