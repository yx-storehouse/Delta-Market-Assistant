"""Recorded whole-layout drift may retry reads; selection gates stay strict."""
import copy
import json
from pathlib import Path
import tempfile
import unittest

from collection_live_session import ForegroundSession, selection_readiness_waiting
from collection_scroll import observe_layout, rebind_selected, selection_plan
import test_collection_live_session as fixtures


ROOT = Path(__file__).resolve().parents[2]


def fresh_packet(value, suffix):
    result = copy.deepcopy(value)
    frame_id, digest = 'synthetic:selection-shift:' + suffix, suffix * 64
    for key in ('collection_observation', 'collection_layout', 'collection_selected_card'):
        result[key]['frame_id'] = frame_id
        result[key]['frame_sha256'] = digest
    result['frames'][-1]['sha256'] = digest
    return result


class RecordedCoherentShiftTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = ROOT / 'artifacts/collection_scroll_speed/windows_01/session.json'
        if not source.is_file():
            raise unittest.SkipTest('local windows_01 recorded packet absent')
        document = json.loads(source.read_text(encoding='utf-8'))
        cls.pending = document['pending_geometry']
        cls.before = document['steps'][-3]['result']
        cls.after = document['steps'][-1]['result']

    def test_actual_packet_only_explains_a_read_retry_and_preserves_original_lease(self):
        frozen = copy.deepcopy((self.after, self.pending))
        result = selection_readiness_waiting(self.after, self.pending)
        self.assertEqual(result['reason'], 'coherent_small_layout_shift_no_selected_card')
        self.assertEqual(result['measured_common_vertical_shift_px'], -2)
        self.assertEqual(result['independent_non_target_peer_count'], 3)
        self.assertEqual(result['layout_member_count'], 8)
        self.assertTrue(result['clipping_edges_unchanged'])
        self.assertTrue(result['requires_original_selected_rebind'])
        self.assertFalse(result['collection_allowed'])
        self.assertFalse(result['selected_observed'])
        self.assertFalse(result['item_identity_proven'])
        self.assertEqual((self.after, self.pending), frozen)
        with self.assertRaisesRegex(ValueError, 'COLLECTION_SELECTION_GEOMETRY_CHANGED'):
            rebind_selected(self.after, self.pending['rule'], self.pending['lease'])

    def test_adding_selected_flag_to_the_shifted_frame_still_does_not_rebind(self):
        value = copy.deepcopy(self.after)
        value['collection_layout']['cards'][2]['selected'] = True
        value['collection_selected_card']['selected'] = True
        self.assertIsNone(selection_readiness_waiting(value, self.pending))
        with self.assertRaisesRegex(ValueError, 'COLLECTION_SELECTION_GEOMETRY_CHANGED'):
            rebind_selected(value, self.pending['rule'], self.pending['lease'])

    def test_missing_incomplete_changed_mask_or_stale_frame_is_not_readiness(self):
        for name in ('missing', 'incomplete', 'visibility', 'stale', 'selected_metadata', 'scrollbar'):
            value = copy.deepcopy(self.after)
            layout = value['collection_layout']
            if name == 'missing':
                layout['cards'].pop()
            elif name == 'incomplete':
                layout['complete'] = False
            elif name == 'visibility':
                layout['cards'][-1]['edges']['right'] = False
            elif name == 'stale':
                for key in ('collection_layout', 'collection_observation', 'collection_selected_card'):
                    value[key]['frame_id'] = self.pending['wait_layout']['frame_id']
            elif name == 'selected_metadata':
                value['collection_selected_card']['frame_id'] = 'stale'
            else:
                layout['scrollbar']['thumb_bounds'][1] += 1
            with self.subTest(name=name):
                self.assertIsNone(selection_readiness_waiting(value, self.pending))

    def test_independent_full_peer_disagreement_or_changed_subroi_is_rejected(self):
        for name in ('top', 'fields', 'condition_roi', 'other_column_selected'):
            value = copy.deepcopy(self.after)
            card = value['collection_layout']['cards'][3]
            if name == 'top':
                card['bounds'][1] += 1
                card['bounds'][3] -= 1
            elif name == 'fields':
                card['fields_bounds'][1] += 1
            elif name == 'condition_roi':
                card['condition_bounds'][0] += 1
            else:
                card['selected'] = True
            with self.subTest(name=name):
                self.assertIsNone(selection_readiness_waiting(value, self.pending))

    def test_partial_observed_edges_and_fixed_clipping_boundaries_are_independently_bounded(self):
        for name in ('horizontal', 'observed_vertical', 'clipped_vertical', 'dehighlight_excess'):
            value = copy.deepcopy(self.after)
            if name == 'horizontal':
                value['collection_layout']['cards'][-1]['bounds'][0] += 1
                value['collection_layout']['cards'][-1]['bounds'][2] -= 1
            elif name == 'observed_vertical':
                value['collection_layout']['cards'][-1]['bounds'][1] -= 2
                value['collection_layout']['cards'][-1]['bounds'][3] += 2
            elif name == 'clipped_vertical':
                value['collection_layout']['cards'][0]['bounds'][1] += 1
                value['collection_layout']['cards'][0]['bounds'][3] -= 1
            else:
                value['collection_layout']['cards'][1]['bounds'][3] -= 1
            with self.subTest(name=name):
                self.assertIsNone(selection_readiness_waiting(value, self.pending))

    def test_shift_over_two_pixels_does_not_increase_the_waiting_contract(self):
        value = copy.deepcopy(self.after)
        for card in value['collection_layout']['cards']:
            if card['edges']['top']:
                card['bounds'][1] -= 1
                if not card['edges']['bottom']:
                    card['bounds'][3] += 1
            else:
                card['bounds'][3] -= 1
            for key in ('fields_bounds', 'condition_bounds', 'price_bounds'):
                if card.get(key) is not None:
                    card[key][1] -= 1
        self.assertIsNone(selection_readiness_waiting(value, self.pending))

    def test_wrong_title_scope_or_prior_target_does_not_enable_waiting(self):
        value = copy.deepcopy(self.after)
        title = next(region for region in value['collection_observation']['regions'] if region['kind'] == 'product_title')
        title['words'][0]['text'] = 'different product'
        self.assertIsNone(selection_readiness_waiting(value, self.pending))
        for name in ('scope', 'target', 'kind'):
            pending = copy.deepcopy(self.pending)
            if name == 'scope':
                pending['lease']['scope']['row_index'] += 1
            elif name == 'target':
                pending['lease']['card']['bounds'][0] += 1
            else:
                pending['kind'] = 'scroll'
            with self.subTest(name=name):
                self.assertIsNone(selection_readiness_waiting(self.after, pending))

    def session_run(self, replies, *, attempts=None):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        clock = fixtures.Clock()
        backend = fixtures.FakeBackend(clock, replies=[copy.deepcopy(self.before), *copy.deepcopy(replies)])
        session = ForegroundSession('artifacts/coherent_shift/session.json', root=Path(temporary.name),
            backend=backend, now=clock.now, wait=clock.wait, fast_settle=True)
        def run():
            with session:
                session.perform(fixtures.capture(collection_layout=True))
                plan = selection_plan(session.previous, self.pending['rule'], self.pending['lease']['card']['id'])
                if attempts is not None:
                    plan['steps'][1]['attempts'] = attempts
                for step in plan['steps']:
                    session.perform(step)
        return session, backend, run

    def test_one_actual_failed_frame_keeps_pending_and_never_claims_selection(self):
        session, backend, run = self.session_run([self.after], attempts=1)
        with self.assertRaisesRegex(RuntimeError, 'BATCH_STEP_FAILED'):
            run()
        self.assertEqual(session.steps[-1]['attempt_count'], 1)
        self.assertTrue(session.steps[-1]['collection_state_retryable'])
        self.assertIsNotNone(session.pending_geometry)
        self.assertIsNone(session.selected_lease)
        self.assertEqual(session.report['manual_clicks'], 1)
        self.assertEqual(list(session.journal.directory.glob('*.json')), [])

    def test_three_distinct_unselected_frames_exhaust_reads_without_reclick_or_favorite(self):
        session, backend, run = self.session_run([self.after, fresh_packet(self.after, 'b'), fresh_packet(self.after, 'c')])
        with self.assertRaisesRegex(RuntimeError, 'BATCH_STEP_FAILED'):
            run()
        self.assertEqual(session.steps[-1]['attempt_count'], 3)
        self.assertTrue(all(attempt['collection_state_retryable'] for attempt in session.steps[-1]['attempts']))
        self.assertIsNotNone(session.pending_geometry)
        self.assertIsNone(session.selected_lease)
        self.assertIsNone(session.pending)
        self.assertEqual(session.report['manual_clicks'], 1)
        self.assertEqual(session.report['leave_calls'], 1)
        self.assertTrue(session.report['ide_restored'])
        self.assertEqual(list(session.journal.directory.glob('*.json')), [])

    def test_only_a_subsequent_original_contract_selected_frame_completes_selection(self):
        # This third frame is explicitly synthetic completion, not a claim
        # that the interrupted real windows_01 run supplied a valid receipt.
        selected = fresh_packet(self.before, 'd')
        target_id = self.pending['lease']['card']['id']
        for card in selected['collection_layout']['cards']:
            card['selected'] = card['id'] == target_id
        selected['collection_selected_card']['selected'] = True
        session, backend, run = self.session_run([self.after, selected])
        run()
        self.assertEqual(session.steps[-1]['attempt_count'], 2)
        self.assertTrue(session.steps[-1]['passed'])
        self.assertIsNone(session.pending_geometry)
        self.assertEqual(session.selected_lease['frame_id'], 'synthetic:selection-shift:d')
        self.assertEqual(session.report['manual_clicks'], 1)
        self.assertEqual(list(session.journal.directory.glob('*.json')), [])


if __name__ == '__main__':
    unittest.main()
