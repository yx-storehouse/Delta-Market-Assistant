"""Run_07 numeric geometry regression; no image files or live input."""
import copy
import hashlib
import json
from pathlib import Path
import unittest
from unittest.mock import patch

from collection_scroll import (bind_card, card_geometry_match, observe_layout,
    rebind_selected, same_card_geometry, validate_card_lease)
from test_collection_scroll import next_frame, packet, rule


ROOT = Path(__file__).resolve().parents[2]
GRAY = dict(bounds=[120, 543, 863, 260], fields_bounds=[121, 760, 861, 42])
WHITE = dict(bounds=[119, 538, 866, 267], fields_bounds=[120, 762, 864, 42])


def observed(card_id, geometry, selected=False):
    return dict(copy.deepcopy(geometry), id=card_id, selected=selected,
                edges=dict(top=True, bottom=True, left=True, right=True))


def pair():
    before = packet()
    before['collection_layout']['listing_viewport'] = [108, 300, 1765, 903]
    neighbor = copy.deepcopy(GRAY)
    neighbor['bounds'][0] += 877
    neighbor['fields_bounds'][0] += 877
    before['collection_layout']['cards'] = [observed('gray', GRAY), observed('neighbor', neighbor)]
    after = next_frame(before)
    after['collection_layout']['cards'][0] = observed('white', WHITE, True)
    return before, after


class VerticalGeometryTests(unittest.TestCase):
    def test_actual_edges_match_symmetrically_without_mutation(self):
        original = copy.deepcopy((GRAY, WHITE))
        for a, b in ((GRAY, WHITE), (WHITE, GRAY)):
            evidence = card_geometry_match(a, b)
            self.assertEqual(evidence['mode'], 'anchored_top_edge')
            self.assertEqual(evidence['smaller_card_coverage'], 1)
            self.assertEqual(evidence['fields_bottom_insets'], [1, 1])
            self.assertEqual(evidence['card_edge_limits'], dict(left=8, top=5, right=8, bottom=3))
            self.assertEqual(evidence['fields_edge_limits'], dict(left=8, top=3, right=8, bottom=3))
            self.assertEqual(evidence['discrepancy_cause'], 'unproven')
            self.assertFalse(evidence['item_identity_proven'])
        self.assertEqual(card_geometry_match(GRAY, WHITE)['card_edge_deltas'],
                         dict(left=-1, top=-5, right=2, bottom=2))
        self.assertEqual((GRAY, WHITE), original)

    def test_top_extension_rejects_lower_band_or_larger_motion(self):
        changes = {
            'top6': lambda c: (c['bounds'].__setitem__(1, 537), c['bounds'].__setitem__(3, 268)),
            'bottom4': lambda c: c['bounds'].__setitem__(3, 269),
            'fields4': lambda c: (c['bounds'].__setitem__(3, 269), c['fields_bounds'].__setitem__(1, 764)),
            'field_height': lambda c: (c['fields_bounds'].__setitem__(1, 761), c['fields_bounds'].__setitem__(3, 43)),
            'field_inset': lambda c: c['fields_bounds'].__setitem__(1, 761),
            'x9': lambda c: c['bounds'].__setitem__(0, 111),
        }
        for label, change in changes.items():
            modified = copy.deepcopy(WHITE)
            change(modified)
            with self.subTest(label=label):
                self.assertFalse(same_card_geometry(GRAY, modified))
                self.assertFalse(same_card_geometry(modified, GRAY))

    def test_top4_and5_require_near_bottom_anchor(self):
        for delta in (4, 5):
            modified = copy.deepcopy(GRAY)
            modified['bounds'][1] -= delta
            modified['bounds'][3] += delta
            self.assertTrue(same_card_geometry(GRAY, modified))
            for geometry in (modified,):
                geometry['fields_bounds'][1] -= 4
            unanchored = copy.deepcopy(GRAY)
            unanchored['fields_bounds'][1] -= 4
            self.assertFalse(same_card_geometry(unanchored, modified))

    def test_full_translation_and_tolerance_override_stay_bounded(self):
        for dx, dy in ((0, 4), (0, -4), (0, 5), (877, 0), (0, 275)):
            other = copy.deepcopy(GRAY)
            for rect in (other['bounds'], other['fields_bounds']):
                rect[0] += dx
                rect[1] += dy
            with self.subTest(dx=dx, dy=dy):
                self.assertFalse(same_card_geometry(GRAY, other))
        self.assertFalse(same_card_geometry(GRAY, WHITE, tolerance=2))
        self.assertFalse(same_card_geometry(GRAY, GRAY, tolerance=4))

    def test_rebind_returns_only_new_lease_and_separate_evidence(self):
        before, after = pair()
        history = copy.deepcopy((before, after))
        prior = bind_card(before, rule(), 'gray')
        result = rebind_selected(after, rule(), prior)
        self.assertEqual(result['lease']['card']['bounds'], WHITE['bounds'])
        self.assertEqual(result['lease']['card']['fields_bounds'], WHITE['fields_bounds'])
        self.assertEqual(result['geometry_match']['matching_card_count'], 1)
        self.assertEqual(validate_card_lease(after, rule(), result['lease']), result['lease'])
        self.assertNotIn('geometry_match', result['lease'])
        self.assertFalse(result['collection_allowed'])
        self.assertFalse(result['item_identity_proven'])
        with self.assertRaises(ValueError):
            validate_card_lease(after, rule(), prior)
        self.assertEqual((before, after), history)

    def test_neighbor_selected_or_stale_capture_rejected(self):
        before, after = pair()
        prior = bind_card(before, rule(), 'gray')
        after['collection_layout']['cards'][0]['selected'] = False
        after['collection_layout']['cards'][1]['selected'] = True
        with self.assertRaisesRegex(ValueError, 'COLLECTION_SELECTION_GEOMETRY_CHANGED'):
            rebind_selected(after, rule(), prior)
        with self.assertRaisesRegex(ValueError, 'COLLECTION_REOBSERVATION_REQUIRED'):
            rebind_selected(before, rule(), prior)

    def test_one_to_one_guard_rejects_multiple_matching_cards(self):
        before, after = pair()
        prior = bind_card(before, rule(), 'gray')
        layout = observe_layout(after)
        duplicate = copy.deepcopy(layout['cards'][0])
        duplicate.update(id='duplicate', selected=False)
        layout['cards'].append(duplicate)
        # The normal layout validator already rejects overlapping cards;
        # the association itself must independently require uniqueness.
        with patch('collection_scroll.observe_layout', return_value=layout):
            with self.assertRaisesRegex(ValueError, 'COLLECTION_SELECTION_GEOMETRY_CHANGED'):
                rebind_selected(after, rule(), prior)

    def test_right_selected_top_offset_does_not_reverse_row_order(self):
        before, _ = pair()
        right = copy.deepcopy(WHITE)
        right['bounds'][0] += 877
        right['fields_bounds'][0] += 877
        before['collection_layout']['cards'][1] = observed('right-selected', right, True)
        self.assertEqual([c['id'] for c in observe_layout(before)['cards']], ['gray', 'right-selected'])
        right['fields_bounds'][1] -= 1  # Lost equal bottom inset: no extension.
        before['collection_layout']['cards'][1] = observed('right-selected', right, True)
        self.assertEqual([c['id'] for c in observe_layout(before)['cards']], ['right-selected', 'gray'])

    def test_processed_gray_and_white_both_cover_returned_gray(self):
        earlier_gray = dict(bounds=[120, 538, 863, 265], fields_bounds=[121, 760, 861, 42])
        self.assertTrue(same_card_geometry(earlier_gray, GRAY))
        self.assertTrue(same_card_geometry(WHITE, GRAY))

    @unittest.skipUnless((ROOT / 'artifacts/collection_full_cycle/run_07/session.json').exists(),
                         'local actual run_07 numeric packet is not packaged')
    def test_actual_run07_offline_replay_preserves_report(self):
        path = ROOT / 'artifacts/collection_full_cycle/run_07/session.json'
        raw = path.read_bytes()
        report = json.loads(raw)
        pending = report['pending_geometry']
        self.assertEqual(pending['kind'], 'selection')
        self.assertIsNone(report['pending_collection'])
        after = report['steps'][-1]['result']
        history = copy.deepcopy((after, pending))
        result = rebind_selected(after, pending['rule'], pending['lease'])
        self.assertEqual(result['lease']['card']['bounds'], WHITE['bounds'])
        self.assertEqual(result['geometry_match']['mode'], 'anchored_top_edge')
        self.assertEqual(result['geometry_match']['matching_card_count'], 1)
        validate_card_lease(after, pending['rule'], result['lease'])
        self.assertEqual((after, pending), history)
        self.assertEqual(hashlib.sha256(raw).digest(), hashlib.sha256(path.read_bytes()).digest())


if __name__ == '__main__':
    unittest.main()
