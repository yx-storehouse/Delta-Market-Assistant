"""No-selected-card transition permits only bounded new observations."""
import copy
import json
from pathlib import Path
import tempfile
import unittest

from collection_live_session import ForegroundSession, selection_readiness_waiting
from collection_scroll import observe_layout, rebind_selected, selection_plan
from test_collection_dynamic import dynamic_packet, rule
import test_collection_live_session as fixtures

ROOT = Path(__file__).resolve().parents[2]


def fresh_id(packet, value):
    result = copy.deepcopy(packet)
    for key in ('collection_observation', 'collection_layout', 'collection_selected_card'):
        result[key]['frame_id'] = value
    return result


class SelectionReadinessSessionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.clock = fixtures.Clock()
        self.backend = fixtures.FakeBackend(self.clock)

    def session(self, **options):
        return ForegroundSession('artifacts/readiness/session.json', root=self.root,
            backend=self.backend, now=self.clock.now, wait=self.clock.wait,
            fast_settle=options.pop('fast_settle', True), **options)

    def run_selection(self, session, *, attempts=None):
        session.perform(fixtures.capture(collection_layout=True))
        plan = selection_plan(session.previous, rule(), 'observed-row')
        if attempts is not None:
            plan['steps'][-1]['attempts'] = attempts
        for step in plan['steps']:
            session.perform(step)

    def test_bounded_outline_change_can_wait_then_original_selected_rebind_succeeds(self):
        self.backend.replies = [dynamic_packet(selected=False),
            dynamic_packet('b', x=996, top=440, selected=False), dynamic_packet('c', x=996, top=440)]
        session = self.session()
        with session:
            self.run_selection(session)
        attempt = session.steps[-1]['attempts'][0]
        self.assertFalse(attempt['passed'])
        self.assertTrue(attempt['collection_state_retryable'])
        self.assertEqual(attempt['result']['collection_geometry_error'], 'COLLECTION_SELECTION_GEOMETRY_CHANGED')
        self.assertFalse(attempt['result']['collection_selection_readiness']['selected_observed'])
        self.assertFalse(attempt['result']['collection_selection_readiness']['collection_allowed'])
        self.assertTrue(session.steps[-1]['passed'])
        self.assertEqual(session.selected_lease['frame_id'], 'fake:c')
        self.assertIsNone(session.pending_geometry)
        self.assertIsNone(session.pending)
        self.assertEqual(session.report['manual_clicks'], 1)

    def test_readiness_without_selection_exhausts_three_observations_and_keeps_pending(self):
        self.backend.replies = [dynamic_packet(selected=False),
            *[dynamic_packet(digest, x=996, top=440, selected=False) for digest in 'bcd'],
            dynamic_packet('e', x=996, top=440)]
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'BATCH_STEP_FAILED'):
            with session:
                self.run_selection(session)
        self.assertEqual(session.steps[-1]['attempt_count'], 3)
        self.assertTrue(all(step['collection_state_retryable'] for step in session.steps[-1]['attempts']))
        self.assertTrue(all(not step['passed'] for step in session.steps[-1]['attempts']))
        self.assertEqual(len(self.backend.replies), 1)
        self.assertIsNotNone(session.pending_geometry)
        self.assertIsNone(session.pending)
        self.assertEqual(session.report['manual_clicks'], 1)
        self.assertEqual(list(session.journal.directory.glob('*.json')), [])
        self.assertEqual(session.report['leave_calls'], 1)
        self.assertTrue(session.report['ide_restored'])

    def test_option_disabled_keeps_original_terminal_selection_failure(self):
        self.backend.replies = [dynamic_packet(selected=False),
            dynamic_packet('b', x=996, top=440, selected=False), dynamic_packet('c')]
        session = self.session(fast_settle=False)
        with self.assertRaisesRegex(RuntimeError, 'BATCH_STEP_FAILED'):
            with session:
                self.run_selection(session)
        self.assertEqual(session.steps[-1]['attempt_count'], 1)
        self.assertFalse(session.steps[-1]['collection_state_retryable'])
        self.assertNotIn('collection_selection_readiness', session.steps[-1]['result'])


class RecordedRun03ReadinessTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = ROOT / 'artifacts/collection_speed/run_03/session.json'
        if not source.is_file():
            raise unittest.SkipTest('local recorded collection_speed/run_03 packet absent')
        cls.document = json.loads(source.read_text(encoding='utf-8'))
        cls.pending = cls.document['pending_geometry']
        cls.before = cls.document['steps'][-3]['result']
        cls.after = cls.document['steps'][-1]['result']

    def test_actual_complete_unselected_packet_is_retry_evidence_not_selection(self):
        self.assertEqual(self.after['collection_observation']['frame_id'],
                         'dxgi:c5fec60f-6ebb-4430-a5aa-4424dcfa4b6a')
        self.assertEqual(self.after['collection_observation']['frame_sha256'],
                         '0f99e2c7db0a0709ce3839c072a0c2406398d3445c06c967c868bffc3c37467d')
        self.assertIsNone(self.document['pending_collection'])
        self.assertEqual(self.pending['kind'], 'selection')
        self.assertEqual(sum(card['selected'] for card in observe_layout(self.after)['cards']), 0)
        frozen = copy.deepcopy((self.after, self.pending))
        readiness = selection_readiness_waiting(self.after, self.pending)
        self.assertEqual(readiness['scope'], 'readiness_retry_only')
        self.assertEqual(readiness['layout_member_count'], 8)
        self.assertEqual(readiness['full_card_count'], 4)
        self.assertEqual(readiness['target_matching_card_count'], 1)
        self.assertFalse(readiness['selected_observed'])
        self.assertFalse(readiness['item_identity_proven'])
        self.assertFalse(readiness['collection_allowed'])
        with self.assertRaisesRegex(ValueError, '^COLLECTION_SELECTION_GEOMETRY_CHANGED$'):
            rebind_selected(self.after, self.pending['rule'], self.pending['lease'])
        self.assertEqual((self.after, self.pending), frozen)

    def test_actual_packet_one_attempt_still_fails_and_does_not_clear_pending(self):
        with tempfile.TemporaryDirectory() as directory:
            clock = fixtures.Clock()
            backend = fixtures.FakeBackend(clock, replies=[copy.deepcopy(self.before), copy.deepcopy(self.after)])
            session = ForegroundSession('artifacts/readiness/session.json', root=Path(directory),
                backend=backend, now=clock.now, wait=clock.wait, fast_settle=True)
            with self.assertRaisesRegex(RuntimeError, 'BATCH_STEP_FAILED'):
                with session:
                    session.perform(fixtures.capture(collection_layout=True))
                    plan = selection_plan(session.previous, self.pending['rule'], self.pending['lease']['card']['id'])
                    session.perform(plan['steps'][0])
                    pending = copy.deepcopy(session.pending_geometry)
                    session.perform(dict(plan['steps'][1], attempts=1))
            self.assertEqual(session.steps[-1]['attempt_count'], 1)
            self.assertTrue(session.steps[-1]['collection_state_retryable'])
            self.assertFalse(session.steps[-1]['passed'])
            self.assertEqual(session.pending_geometry, pending)
            self.assertIsNone(session.pending)
            self.assertIsNone(session.selected_lease)
            self.assertEqual(session.report['manual_clicks'], 1)
            self.assertEqual(list(session.journal.directory.glob('*.json')), [])
            self.assertTrue(session.report['ide_restored'])

    def test_previous_frame_id_and_stale_selection_packet_are_rejected(self):
        stale = fresh_id(self.after, self.pending['wait_layout']['frame_id'])
        self.assertIsNone(selection_readiness_waiting(stale, self.pending))
        for key, value in (('frame_id', 'stale'), ('frame_sha256', 'a'*64), ('same_frame', False)):
            packet = copy.deepcopy(self.after)
            packet['collection_selected_card'][key] = value
            with self.subTest(key=key):
                self.assertIsNone(selection_readiness_waiting(packet, self.pending))

    def test_other_selected_card_or_conflicting_selected_packet_is_rejected(self):
        other_selected = copy.deepcopy(self.after)
        other_selected['collection_layout']['cards'][3]['selected'] = True
        self.assertIsNone(selection_readiness_waiting(other_selected, self.pending))
        selected_packet = copy.deepcopy(self.after)
        selected_packet['collection_selected_card']['selected'] = True
        self.assertIsNone(selection_readiness_waiting(selected_packet, self.pending))

    def test_scrollbar_or_viewport_change_is_rejected(self):
        for changed in ('scrollbar', 'listing_viewport', 'viewport'):
            packet = copy.deepcopy(self.after)
            layout = packet['collection_layout']
            if changed == 'scrollbar':
                layout['scrollbar']['thumb_bounds'][1] += 1
            else:
                layout[changed][0] += 1
            with self.subTest(changed=changed):
                self.assertIsNone(selection_readiness_waiting(packet, self.pending))

    def test_title_mismatch_or_incomplete_layout_is_rejected(self):
        wrong_title = copy.deepcopy(self.after)
        region = next(region for region in wrong_title['collection_observation']['regions']
                      if region['kind'] == 'product_title')
        region['words'][0]['text'] = 'not the pending product'
        self.assertIsNone(selection_readiness_waiting(wrong_title, self.pending))
        incomplete = copy.deepcopy(self.after)
        incomplete['collection_layout']['complete'] = False
        self.assertIsNone(selection_readiness_waiting(incomplete, self.pending))

    def test_target_or_other_full_member_geometry_conflict_is_rejected(self):
        for index in (2, 3):
            packet = copy.deepcopy(self.after)
            packet['collection_layout']['cards'][index]['bounds'][1] += 10
            with self.subTest(index=index):
                self.assertIsNone(selection_readiness_waiting(packet, self.pending))
        subroi = copy.deepcopy(self.after)
        subroi['collection_layout']['cards'][3]['condition_bounds'][0] += 3
        self.assertIsNone(selection_readiness_waiting(subroi, self.pending))

    def test_missing_member_or_partial_change_over_one_pixel_is_rejected(self):
        missing = copy.deepcopy(self.after)
        missing['collection_layout']['cards'].pop()
        self.assertIsNone(selection_readiness_waiting(missing, self.pending))
        partial = copy.deepcopy(self.after)
        partial['collection_layout']['cards'][-1]['bounds'][2] -= 2
        self.assertIsNone(selection_readiness_waiting(partial, self.pending))
        visibility = copy.deepcopy(self.after)
        visibility['collection_layout']['cards'][0]['edges']['top'] = True
        self.assertIsNone(selection_readiness_waiting(visibility, self.pending))

    def test_wrong_pending_kind_scope_or_unbound_target_is_rejected(self):
        for mutate in ('kind', 'scope', 'target'):
            pending = copy.deepcopy(self.pending)
            if mutate == 'kind':
                pending['kind'] = 'scroll'
            elif mutate == 'scope':
                pending['lease']['scope']['condition_label'] = '成色S'
            else:
                pending['lease']['card']['bounds'][0] -= 1
            with self.subTest(mutate=mutate):
                self.assertIsNone(selection_readiness_waiting(self.after, pending))


if __name__ == '__main__':
    unittest.main()
