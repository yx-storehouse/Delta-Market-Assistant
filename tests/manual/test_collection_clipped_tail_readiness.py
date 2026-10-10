"""Recorded fast-capture transition: read again, never accept a missing selection."""
import copy
import json
from pathlib import Path
import tempfile
import unittest

from collection_live_session import ForegroundSession, selection_readiness_waiting
from collection_scroll import rebind_selected, selection_plan
import test_collection_live_session as fake

FIXTURE = Path(__file__).resolve().parents[1] / 'fixtures/collection_selection_clipped_tail.json'


class ClippedTailReadinessTests(unittest.TestCase):
    def setUp(self):
        self.data = json.loads(FIXTURE.read_text('utf8'))
        self.after = self.data['after']
        self.pending = self.data['pending']

    def test_recorded_unresolved_tail_allows_only_fresh_observation(self):
        original = copy.deepcopy(self.data)
        result = selection_readiness_waiting(self.after, self.pending)
        self.assertEqual(result['reason'], 'clipped_tail_boundary_no_selected_card')
        self.assertFalse(result['unique_member_geometry_matches'])
        self.assertTrue(result['unique_full_card_geometry_matches'])
        self.assertFalse(result['collection_allowed'])
        self.assertFalse(result['unresolved_clipped_tail_edges'][0]['geometry_accepted'])
        with self.assertRaisesRegex(ValueError, 'SELECTION_GEOMETRY_CHANGED'):
            rebind_selected(self.after, self.pending['rule'], self.pending['lease'])
        self.assertEqual(self.data, original)

    def test_bounded_retry_does_not_dispatch_another_click_or_create_a_lease(self):
        with tempfile.TemporaryDirectory() as directory:
            clock = fake.Clock()
            backend = fake.FakeBackend(clock, [self.data['before'], self.after, self.after])
            session = ForegroundSession('artifacts/test/session.json', root=Path(directory),
                backend=backend, now=clock.now, wait=clock.wait, fast_settle=True)
            with self.assertRaisesRegex(RuntimeError, 'BATCH_STEP_FAILED'):
                with session:
                    session.perform(fake.capture(collection_layout=True))
                    plan = selection_plan(session.previous, self.pending['rule'], self.pending['lease']['card']['id'])
                    session.perform(plan['steps'][0])
                    pending = copy.deepcopy(session.pending_geometry)
                    session.perform(dict(plan['steps'][1], attempts=2))
            self.assertEqual(session.steps[-1]['attempt_count'], 2)
            self.assertTrue(session.steps[-1]['capture_attempts'][0]['collection_state_retryable'])
            self.assertEqual(session.pending_geometry, pending)
            self.assertIsNone(session.selected_lease)
            self.assertIsNone(session.pending)
            self.assertEqual(session.report['manual_clicks'], 1)
            self.assertEqual(list(session.journal.directory.glob('*.json')), [])
            self.assertTrue(session.report['ide_restored'])

    def test_fresh_settled_selection_must_pass_original_rebind(self):
        settled = copy.deepcopy(self.data['before'])
        card_id = self.pending['lease']['card']['id']
        for card in settled['collection_layout']['cards']:
            card['selected'] = card['id'] == card_id
        target = next(card for card in settled['collection_layout']['cards'] if card['selected'])
        for section in ('collection_layout', 'collection_observation', 'collection_selected_card'):
            settled[section].update(frame_id='fresh:settled', frame_sha256='f' * 64)
        settled['frames'][-1]['sha256'] = 'f' * 64
        settled['collection_selected_card'].update(selected=True, card_id=card_id,
            bounds=target['bounds'], fields_bounds=target['fields_bounds'])
        with tempfile.TemporaryDirectory() as directory:
            clock = fake.Clock()
            backend = fake.FakeBackend(clock, [self.data['before'], self.after, settled])
            session = ForegroundSession('artifacts/test/session.json', root=Path(directory),
                backend=backend, now=clock.now, wait=clock.wait, fast_settle=True)
            with session:
                session.perform(fake.capture(collection_layout=True))
                plan = selection_plan(session.previous, self.pending['rule'], card_id)
                for step in plan['steps']:
                    session.perform(step)
            self.assertEqual(session.steps[-1]['attempt_count'], 2)
            self.assertIsNone(session.pending_geometry)
            self.assertEqual(session.selected_lease['frame_id'], 'fresh:settled')
            self.assertEqual(session.report['manual_clicks'], 1)
            self.assertEqual(list(session.journal.directory.glob('*.json')), [])

    def test_other_drift_masks_identity_or_selection_do_not_gain_retry(self):
        for mutation in ('not_boundary', 'tail_left', 'tail_top', 'tail_basis', 'tail_right_observed',
                         'target_moved', 'scrollbar', 'other_selected', 'title', 'stale'):
            packet = copy.deepcopy(self.after)
            tail = packet['collection_layout']['cards'][-1]
            if mutation == 'not_boundary': tail['bounds'][2] -= 1
            elif mutation == 'tail_left': tail['bounds'][0] += 1; tail['bounds'][2] -= 1
            elif mutation == 'tail_top': tail['bounds'][1] += 4; tail['bounds'][3] -= 4
            elif mutation == 'tail_basis': tail['bounds_basis'] = 'observed'
            elif mutation == 'tail_right_observed': tail['edges']['right'] = True
            elif mutation == 'target_moved': packet['collection_layout']['cards'][1]['bounds'][1] += 10
            elif mutation == 'scrollbar': packet['collection_layout']['scrollbar']['thumb_bounds'][1] += 1
            elif mutation == 'other_selected': packet['collection_layout']['cards'][0]['selected'] = True
            elif mutation == 'title':
                next(r for r in packet['collection_observation']['regions'] if r['kind'] == 'product_title')['words'][0]['text'] = 'other product'
            else: packet['collection_selected_card']['frame_id'] = 'old'
            with self.subTest(mutation=mutation):
                self.assertIsNone(selection_readiness_waiting(packet, self.pending))


if __name__ == '__main__':
    unittest.main()
