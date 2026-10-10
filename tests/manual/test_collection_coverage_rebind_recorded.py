"""Recorded numerical geometry replay; no new capture, input, or image files."""
import copy
import json
from pathlib import Path
import unittest

from collection_scroll import build_scroll_coverage, same_card_geometry


class RecordedCoverageRebindTests(unittest.TestCase):
    def setUp(self):
        path = Path(__file__).resolve().parents[1] / 'runtime/fixtures/collection_coverage_run05.json'
        self.data = json.loads(path.read_text('utf8'))
        self.packet, self.rule, self.items = (self.data[k] for k in ('packet', 'rule', 'processed_cards'))

    def test_run05_uses_verified_candidate_not_superseded_gray_top(self):
        original = copy.deepcopy(self.data)
        old, candidate = self.items[3]['card'], self.items[3]['candidate']
        current = self.packet['collection_layout']['cards'][3]
        self.assertFalse(same_card_geometry(old, current))
        self.assertTrue(same_card_geometry(old, candidate))
        self.assertTrue(same_card_geometry(candidate, current))
        coverage = build_scroll_coverage(self.packet, self.rule, self.items)
        self.assertEqual(len(coverage['current_cards']), 6)
        self.assertEqual(len({c['card_id'] for c in coverage['current_cards']}), 6)
        self.assertEqual(self.data, original)
        self.assertFalse(coverage['cross_scroll_identity_reuse'])

    def test_original_must_still_match_actual_selected_candidate(self):
        self.items[3]['card']['bounds'][1] -= 20
        self.items[3]['card']['bounds'][3] += 20
        with self.assertRaisesRegex(ValueError, 'COVERAGE_CANDIDATE'):
            build_scroll_coverage(self.packet, self.rule, self.items)

    def test_latest_candidate_to_current_field_drift_is_not_relaxed(self):
        self.items[3]['candidate']['fields_bounds'][1] -= 4
        with self.assertRaisesRegex(ValueError, 'COVERAGE_'):
            build_scroll_coverage(self.packet, self.rule, self.items)

    def test_duplicate_processed_item_does_not_cover_unseen_item(self):
        self.items[-1] = copy.deepcopy(self.items[0])
        with self.assertRaisesRegex(ValueError, 'COVERAGE_BINDING'):
            build_scroll_coverage(self.packet, self.rule, self.items)

    def test_chain_cannot_hide_cumulative_lower_band_movement(self):
        # Make the source/candidate association individually plausible while
        # its original lower band is >3px away from the present lower band.
        old = self.items[3]['card']
        old['bounds'][3] -= 4
        old['fields_bounds'][1] -= 4
        self.assertTrue(same_card_geometry(old, self.items[3]['candidate']))
        with self.assertRaisesRegex(ValueError, 'COVERAGE_'):
            build_scroll_coverage(self.packet, self.rule, self.items)


if __name__ == '__main__':
    unittest.main()
