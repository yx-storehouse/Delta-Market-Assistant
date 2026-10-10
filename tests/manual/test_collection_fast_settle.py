"""Bounded readiness reads for fast dynamic actions; no game or OS inputs."""
import copy
import json
from pathlib import Path
import tempfile
import unittest

from collection_live_session import ForegroundSession, unchanged_layout_waiting
from collection_scroll import observe_layout, selection_plan, scroll_plan
from test_collection_dynamic import dynamic_packet, rule, transient_packet
import test_collection_live_session as fixtures


def same_pixels_new_frame(source, frame_id):
    value = copy.deepcopy(source)
    for key in ('collection_observation', 'collection_layout', 'collection_selected_card'):
        value[key]['frame_id'] = frame_id
    return value


class FastSettleTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        path = self.root / 'artifacts/trial/input/snapshot.json'
        path.parent.mkdir(parents=True)
        path.write_text(json.dumps(fixtures.snapshot()), encoding='utf-8')
        self.clock = fixtures.Clock()
        self.backend = fixtures.FakeBackend(self.clock)

    def session(self, **kwargs):
        return ForegroundSession('artifacts/trial/session.json', root=self.root,
            backend=self.backend, now=self.clock.now, wait=self.clock.wait,
            fast_settle=kwargs.pop('fast_settle', True), **kwargs)

    def selection(self, session):
        return selection_plan(session.previous, rule(), 'observed-row')['steps']

    def assert_one_action(self, session):
        self.assertEqual(len([event for event in self.backend.events
            if isinstance(event, tuple) and event[0] in ('click', 'scroll')]), 1)
        self.assertEqual(session.report['leave_calls'], 1)
        self.assertTrue(session.report['ide_restored'])

    def test_selection_requests_immediate_frame_then_existing_full_geometry_gate(self):
        self.backend.replies = [dynamic_packet(selected=False), dynamic_packet('b')]
        session = self.session()
        with session:
            session.perform(fixtures.capture(collection_layout=True))
            for step in self.selection(session):
                session.perform(step)
            self.assertIsNone(session.pending_geometry)
            self.assertEqual(session.selected_lease['frame_id'], 'fake:b')
        action = session.steps[1]
        self.assertEqual(action['timings']['settle_wait_ms'], 0)
        self.assertEqual(action['settle_policy'], 'immediate_fresh_readback_v2')
        self.assertEqual(action['post_click_fixed_wait_ms'], 0)
        self.assert_one_action(session)

    def test_scroll_stream_readiness_is_internal_only_for_dispatched_wheel(self):
        self.backend.ready_stream = True
        self.backend.reuse_capture = True
        self.backend.replies = [dynamic_packet(gold=True), dynamic_packet('b', gold=True, thumb_y=370)]
        session = self.session()
        with session:
            first = session.perform(fixtures.capture(collection_layout=True))
            self.assertNotIn('--collection-scroll-readiness', first['command'])
            for step in scroll_plan(session.previous, rule(), [], delta=-120)['steps']:
                last = session.perform(step)
            self.assertIn('--collection-scroll-readiness', last['command'])
            self.assertNotIn('--collection-selection-preflight', last['command'])
            self.assertIsNone(session.pending_geometry)
        self.assert_one_action(session)

    def test_caller_cannot_supply_scroll_readiness_flag(self):
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'COLLECTION_SCROLL_READINESS_INTERNAL'):
            with session:
                session.perform(fixtures.capture(collection_layout=True, scroll_readiness=True))
        self.assertEqual(self.backend.replies, [])

    def test_unchanged_selection_gets_new_read_not_another_click(self):
        self.backend.replies = [dynamic_packet(selected=False), dynamic_packet('b', selected=False), dynamic_packet('c')]
        session = self.session()
        with session:
            session.perform(fixtures.capture(collection_layout=True))
            for step in self.selection(session):
                session.perform(step)
        self.assertEqual(session.steps[-1]['attempt_count'], 2)
        self.assertTrue(session.steps[-1]['attempts'][0]['collection_state_retryable'])
        self.assertIsNone(session.pending_geometry)
        self.assert_one_action(session)

    def test_identical_pixels_need_independent_capture_id_then_actual_selection(self):
        before = dynamic_packet(selected=False)
        waiting = same_pixels_new_frame(before, 'fresh-same-pixels')
        self.backend.replies = [before, waiting, dynamic_packet('c')]
        session = self.session()
        with session:
            session.perform(fixtures.capture(collection_layout=True))
            for step in self.selection(session):
                session.perform(step)
        self.assertEqual(session.steps[-1]['attempt_count'], 2)
        self.assertTrue(session.steps[-1]['attempts'][0]['collection_state_retryable'])
        self.assertEqual(session.selected_lease['frame_id'], 'fake:c')
        self.assert_one_action(session)

    def test_waiting_frame_cannot_reuse_original_id(self):
        self.backend.replies = [dynamic_packet(selected=False), dynamic_packet(selected=False), dynamic_packet('c')]
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'BATCH_STEP_FAILED'):
            with session:
                session.perform(fixtures.capture(collection_layout=True))
                for step in self.selection(session):
                    session.perform(step)
        self.assertEqual(session.steps[-1]['attempt_count'], 1)
        self.assertIsNotNone(session.pending_geometry)
        self.assert_one_action(session)

    def test_waiting_capture_id_cannot_be_repeated(self):
        self.backend.replies = [dynamic_packet(selected=False), dynamic_packet('b', selected=False),
                                dynamic_packet('b', selected=False), dynamic_packet('c')]
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'BATCH_STEP_FAILED'):
            with session:
                session.perform(fixtures.capture(collection_layout=True))
                for step in self.selection(session):
                    session.perform(step)
        self.assertEqual(session.steps[-1]['attempt_count'], 2)
        self.assertFalse(session.steps[-1]['attempts'][-1]['collection_state_retryable'])
        self.assert_one_action(session)

    def test_stasis_and_old_edge_retry_share_three_attempt_limit(self):
        self.backend.replies = [dynamic_packet(selected=False), dynamic_packet('b', selected=False),
                                transient_packet('c'), dynamic_packet('d', selected=False), dynamic_packet('e')]
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'BATCH_STEP_FAILED'):
            with session:
                session.perform(fixtures.capture(collection_layout=True))
                for step in self.selection(session):
                    session.perform(step)
        self.assertEqual(session.steps[-1]['attempt_count'], 3)
        self.assertEqual(len(self.backend.replies), 1)
        self.assertIsNotNone(session.pending_geometry)
        self.assert_one_action(session)

    def test_selection_drift_stops_without_waiting(self):
        self.backend.replies = [dynamic_packet(selected=False), dynamic_packet('b', top=430, selected=False), dynamic_packet('c')]
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'BATCH_STEP_FAILED'):
            with session:
                session.perform(fixtures.capture(collection_layout=True))
                for step in self.selection(session):
                    session.perform(step)
        self.assertEqual(session.steps[-1]['attempt_count'], 1)
        self.assertFalse(session.steps[-1]['collection_state_retryable'])
        self.assert_one_action(session)

    def test_pending_selection_still_blocks_new_action(self):
        self.backend.replies = [dynamic_packet(selected=False)]
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'COLLECTION_GEOMETRY_REOBSERVATION_REQUIRED'):
            with session:
                session.perform(fixtures.capture(collection_layout=True))
                session.perform(self.selection(session)[0])
                session.perform(fixtures.collect())
        self.assert_one_action(session)

    def test_white_star_waits_then_confirms_one_dispatched_action(self):
        self.backend.replies = [dynamic_packet(), dynamic_packet('b'), dynamic_packet('c', gold=True)]
        session = self.session()
        with session:
            session.perform(fixtures.capture(collection_layout=True))
            action = session.perform(fixtures.collect())
            self.assertEqual(action['timings']['settle_wait_ms'], 0)
            self.assertEqual(action['settle_policy'], 'immediate_fresh_readback_v2')
            result = session.perform(fixtures.capture(collection_layout=True, expect_collection_added=True))
        self.assertEqual(result['attempt_count'], 2)
        self.assertTrue(result['attempts'][0]['collection_state_retryable'])
        document = json.loads(next(session.journal.directory.glob('*.json')).read_text('utf-8'))
        self.assertEqual(document['status'], 'confirmed')
        self.assertIsNone(session.pending)
        self.assert_one_action(session)

    def test_white_star_stalls_keep_dispatched_journal(self):
        self.backend.replies = [dynamic_packet(), dynamic_packet('b'), dynamic_packet('c'), dynamic_packet('d'), dynamic_packet('e', gold=True)]
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'BATCH_STEP_FAILED'):
            with session:
                session.perform(fixtures.capture(collection_layout=True))
                session.perform(fixtures.collect())
                session.perform(fixtures.capture(collection_layout=True, expect_collection_added=True))
        self.assertEqual(session.steps[-1]['attempt_count'], 3)
        document = json.loads(next(session.journal.directory.glob('*.json')).read_text('utf-8'))
        self.assertEqual(document['status'], 'dispatched')
        self.assertIsNotNone(session.pending)
        self.assert_one_action(session)

    def test_changed_price_is_conflict_not_waiting_white_star(self):
        changed = dynamic_packet('b')
        changed['collection_observation']['regions'][-1]['words'][-1]['text'] = '231'
        self.backend.replies = [dynamic_packet(), changed, dynamic_packet('c', gold=True)]
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'BATCH_STEP_FAILED'):
            with session:
                session.perform(fixtures.capture(collection_layout=True))
                session.perform(fixtures.collect())
                session.perform(fixtures.capture(collection_layout=True, expect_collection_added=True))
        self.assertEqual(session.steps[-1]['attempt_count'], 1)
        self.assertFalse(session.steps[-1]['collection_state_retryable'])
        self.assertIsNotNone(session.pending)
        self.assert_one_action(session)

    def test_same_pixels_white_can_wait_but_never_confirm_without_gold(self):
        before = dynamic_packet()
        self.backend.replies = [before, same_pixels_new_frame(before, 'fresh-white'), dynamic_packet('c', gold=True)]
        session = self.session()
        with session:
            session.perform(fixtures.capture(collection_layout=True))
            session.perform(fixtures.collect())
            result = session.perform(fixtures.capture(collection_layout=True, expect_collection_added=True))
        self.assertFalse(result['attempts'][0]['result']['collection_receipt_passed'])
        self.assertEqual(result['attempt_count'], 2)
        self.assert_one_action(session)

    def test_unknown_star_color_is_not_readiness_retry(self):
        unknown = dynamic_packet('b')
        unknown['collection_selected_card']['favorite_warm_fraction'] = .03
        self.backend.replies = [dynamic_packet(), unknown, dynamic_packet('c', gold=True)]
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'BATCH_STEP_FAILED'):
            with session:
                session.perform(fixtures.capture(collection_layout=True))
                session.perform(fixtures.collect())
                session.perform(fixtures.capture(collection_layout=True, expect_collection_added=True))
        self.assertEqual(session.steps[-1]['attempt_count'], 1)
        self.assert_one_action(session)

    def test_scroll_unchanged_thumb_waits_then_requires_original_progress_proof(self):
        self.backend.replies = [dynamic_packet(gold=True), dynamic_packet('b', gold=True),
                                dynamic_packet('c', gold=True, thumb_y=370)]
        session = self.session()
        with session:
            session.perform(fixtures.capture(collection_layout=True))
            plan = scroll_plan(session.previous, rule(), [], delta=-120)
            for step in plan['steps']:
                session.perform(step)
        self.assertEqual(session.steps[1]['timings']['settle_wait_ms'], 180)
        self.assertEqual(session.steps[-1]['attempt_count'], 2)
        self.assertEqual(session.previous['collection_geometry_rebind']['evidence']['scrollbar_shift_pixels'], 20)
        self.assertFalse(session.previous['collection_geometry_rebind']['evidence']['page_exhausted'])
        self.assert_one_action(session)

    def test_foreground_loss_during_readiness_wait_restores_once_no_new_read(self):
        self.backend.replies = [dynamic_packet(selected=False), dynamic_packet('b', selected=False), dynamic_packet('c')]
        session = self.session()
        original_wait = session.wait
        def lose_focus(seconds):
            original_wait(seconds)
            if abs(seconds - .15) < 1e-6:
                self.backend.active = False
        session.wait = lose_focus
        with self.assertRaisesRegex(RuntimeError, 'BATCH_FOREGROUND_LOST'):
            with session:
                session.perform(fixtures.capture(collection_layout=True))
                for step in self.selection(session):
                    session.perform(step)
        self.assertEqual(len(self.backend.replies), 1)
        self.assert_one_action(session)

    def test_coordinator_roundtrip_still_counts_toward_five_second_limit(self):
        self.backend.replies = [dynamic_packet(selected=False), dynamic_packet('b', selected=False), dynamic_packet('c')]
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'BATCH_STEP_FAILED'):
            with session:
                session.perform(fixtures.capture(collection_layout=True))
                steps = self.selection(session)
                session.perform(steps[0])
                self.backend.capture_seconds = 5.1
                session.perform(steps[1])
        self.assertEqual(session.steps[-1]['attempt_count'], 1)
        self.assertFalse(session.steps[-1]['collection_state_retryable'])
        self.assert_one_action(session)

    def test_non_dynamic_navigation_keeps_existing_settle(self):
        self.backend.replies = [dynamic_packet()]
        session = self.session()
        with session:
            session.perform(fixtures.capture(collection_layout=True))
            action = session.perform(dict(kind='click', expected_before='skin_listings', point=[168, 1403]))
        self.assertEqual(action['timings']['settle_wait_ms'], 600)
        self.assertEqual(action['settle_policy'], 'fixed')

    def test_old_default_is_unchanged_and_extra_options_are_typed(self):
        self.backend.replies = [dynamic_packet(), dynamic_packet('b', gold=True)]
        session = self.session(fast_settle=False)
        with session:
            session.perform(fixtures.capture(collection_layout=True))
            action = session.perform(fixtures.collect())
            session.perform(fixtures.capture(collection_layout=True, expect_collection_added=True))
        self.assertEqual(action['timings']['settle_wait_ms'], 600)
        for options, error in ((dict(fast_settle=1), 'FAST_SETTLE_OPTION'),
                               (dict(persistent_capture=1), 'CAPTURE_WORKER_OPTION')):
            with self.subTest(options=options), self.assertRaisesRegex(RuntimeError, error):
                ForegroundSession('artifacts/unused.json', root=self.root, **options)

    def test_partial_malformed_and_changed_scope_are_not_stasis(self):
        original = dynamic_packet(selected=False)
        pending = dict(wait_layout=observe_layout(original))
        changes = [transient_packet('b'), dynamic_packet('b', top=430, selected=False)]
        wrong_scope = dynamic_packet('b', selected=False)
        wrong_scope['startup_page']['overlay'] = 'listing_filter'
        changes.append(wrong_scope)
        for packet in changes:
            self.assertFalse(unchanged_layout_waiting(packet, pending))


if __name__ == '__main__':
    unittest.main()
