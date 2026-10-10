"""The scrollbar's one-pixel centreline may move to the adjacent column.

Recorded frame (tests/fixtures): hotkey run 2026-10-09 09:13, row 14 AS Val,
after its first window of six favorites. The native scrollbar centreline read
x 1879 while all four measured profiles were taken at x 1878, so every profile
was rejected and the run stopped before scrolling. Only x moved; the track's
vertical extent and the thumb were unchanged. No game, no input.
"""
import copy
import json
from pathlib import Path
import unittest

import collection_live_session as live
from collection_scroll import (build_scroll_coverage, observe_layout, rebind_after_scroll,
    same_scrollbar_position, same_scrollbar_track, scroll_plan, select_window_scroll_calibration)
from test_collection_scroll import rule
from test_collection_scroll_adaptive_bank import after_shifted_window, five_notches, shifted_window
from test_collection_batch_scroll import processed

FIXTURE = Path(__file__).resolve().parents[1] / 'fixtures' / 'hotkey_scrollbar_centreline.json'


def move_bar(packet, dx):
    bar = packet['collection_layout']['scrollbar']
    for key in ('track_bounds', 'thumb_bounds'):
        bar[key][0] += dx
    return packet


class RecordedCentrelineTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.recorded = json.loads(FIXTURE.read_text('utf-8'))

    def choose(self, dx=0):
        packet = move_bar(copy.deepcopy(self.recorded['packet']), dx)
        row = self.recorded['rule']
        coverage = build_scroll_coverage(packet, row, copy.deepcopy(self.recorded['processed']))
        return select_window_scroll_calibration(packet, row, coverage, copy.deepcopy(self.recorded['profiles']))

    def test_recorded_frame_selects_the_profile_measured_for_its_thumb(self):
        self.assertEqual(observe_layout(self.recorded['packet'])['scrollbar']['track_bounds'][0], 1879)
        profile = self.choose()
        self.assertEqual(profile['delta'], -480)
        self.assertEqual(profile['scrollbar']['thumb_height_range'], [61, 63])
        self.assertEqual(self.choose(-2)['delta'], -480)  # x 1877: one column on the other side

    def test_two_columns_away_is_still_a_different_bar(self):
        with self.assertRaisesRegex(ValueError, '^COLLECTION_SCROLL_CALIBRATION_NO_SAFE_PROFILE$') as failure:
            self.choose(1)
        self.assertTrue(all(item['reason'] == 'COLLECTION_SCROLL_CALIBRATION_SCROLLBAR'
                            for item in failure.exception.candidate_rejections))

    def test_a_different_thumb_height_is_never_accepted(self):
        packet = copy.deepcopy(self.recorded['packet'])
        packet['collection_layout']['scrollbar']['thumb_bounds'][3] = 66
        row = self.recorded['rule']
        coverage = build_scroll_coverage(packet, row, copy.deepcopy(self.recorded['processed']))
        with self.assertRaisesRegex(ValueError, '^COLLECTION_SCROLL_CALIBRATION_NO_SAFE_PROFILE$'):
            select_window_scroll_calibration(packet, row, coverage, copy.deepcopy(self.recorded['profiles']))


class RebindCentrelineTests(unittest.TestCase):
    def lease(self):
        before = shifted_window()
        coverage = build_scroll_coverage(before, rule(), processed(before))
        profile = five_notches()
        plan = scroll_plan(before, rule(), [], delta=-600, calibration=profile, coverage=coverage)
        return before, plan['steps'][0]['lease']

    def test_scroll_rebind_accepts_the_adjacent_centreline(self):
        before, lease = self.lease()
        for dx in (-1, 1):
            rebound = rebind_after_scroll(move_bar(after_shifted_window(before), dx), rule(), lease)
            self.assertEqual(rebound['batch_scroll_evidence']['displacement_interval_pixels'], [732, 738])

    def test_scroll_rebind_still_rejects_a_moved_track_or_thumb_column(self):
        before, lease = self.lease()
        with self.assertRaisesRegex(ValueError, '^COLLECTION_SCROLL_TRACK_CHANGED$'):
            rebind_after_scroll(move_bar(after_shifted_window(before), 2), rule(), lease)
        taller = after_shifted_window(before)
        taller['collection_layout']['scrollbar']['track_bounds'][3] -= 1
        with self.assertRaisesRegex(ValueError, '^COLLECTION_SCROLL_TRACK_CHANGED$'):
            rebind_after_scroll(taller, rule(), lease)


class StasisCentrelineTests(unittest.TestCase):
    def test_position_comparison(self):
        bar = dict(track_bounds=[1878, 304, 1, 899], thumb_bounds=[1878, 304, 1, 62])
        jitter = dict(track_bounds=[1879, 304, 1, 899], thumb_bounds=[1879, 304, 1, 62])
        self.assertTrue(same_scrollbar_track(bar['track_bounds'], jitter['track_bounds']))
        self.assertTrue(same_scrollbar_position(bar, jitter))
        self.assertTrue(same_scrollbar_position(None, None))
        self.assertFalse(same_scrollbar_position(bar, None))
        moved = dict(track_bounds=[1879, 304, 1, 899], thumb_bounds=[1879, 305, 1, 62])
        self.assertFalse(same_scrollbar_position(bar, moved))
        skewed = dict(track_bounds=[1879, 304, 1, 899], thumb_bounds=[1878, 304, 1, 62])
        self.assertFalse(same_scrollbar_position(bar, skewed))

    def test_unchanged_layout_wait_tolerates_only_the_centreline(self):
        recorded = json.loads(FIXTURE.read_text('utf-8'))['packet']
        before = observe_layout(recorded)
        later = json.loads(json.dumps(recorded).replace(before['frame_id'], 'dxgi:later-frame'))
        self.assertTrue(live.unchanged_layout_waiting(move_bar(copy.deepcopy(later), -1), dict(wait_layout=before)))
        later['collection_layout']['scrollbar']['thumb_bounds'][1] += 3
        self.assertFalse(live.unchanged_layout_waiting(later, dict(wait_layout=before)))


if __name__ == '__main__':
    unittest.main()
