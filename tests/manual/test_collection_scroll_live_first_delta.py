"""Offline tests of the read-only verifier's measured first-distance option."""
import copy
import json
from pathlib import Path
import unittest

from collection_scroll import build_scroll_coverage, scroll_plan, validate_scroll_lease
from run_collection_trial import load_scroll_profile_bank
from test_collection_batch_scroll import calibration, processed
from test_collection_scroll import packet, rule
from test_collection_scroll_adaptive_bank import after_shifted_window, five_notches, shifted_window
from verify_collection_scroll_live import choose_window_profile, price_independent_observation_key


class FirstMeasuredDeltaTests(unittest.TestCase):
    def test_observation_composite_normalizes_wear_and_ignores_price_without_claiming_server_id(self):
        before = dict(product='fixture-product', condition='fixture-condition', price='300', wear='0.1200')
        after = dict(before, price='250', wear='0.12')
        self.assertEqual(price_independent_observation_key(before), price_independent_observation_key(after))
        self.assertEqual(price_independent_observation_key(before), ('fixture-product', 'fixture-condition', '0.12'))

    def choose(self, value=None, profiles=None, *, index=0, first_delta=None):
        value = value or packet()
        coverage = build_scroll_coverage(value, rule(), processed(value, 'read_only_observed'),
                                         mode='read_only_probe')
        chosen = choose_window_profile(value, rule(), coverage, profiles or [calibration(), five_notches()],
                                       window_index=index, first_delta=first_delta)
        return chosen, coverage

    def test_default_still_chooses_shortest_safe_measured_profile(self):
        chosen, _ = self.choose()
        self.assertEqual(chosen['delta'], -480)

    def test_explicit_first_five_uses_existing_calibration_and_same_coverage_plan(self):
        chosen, coverage = self.choose(first_delta=-600)
        self.assertEqual(chosen, five_notches())
        plan = scroll_plan(packet(), rule(), [], delta=chosen['delta'], calibration=chosen, coverage=coverage)
        lease = plan['steps'][0]['lease']
        self.assertEqual(validate_scroll_lease(packet(), rule(), [], lease), lease)
        self.assertEqual(lease['coverage']['mode'], 'read_only_probe')
        self.assertFalse(lease['coverage']['collection_completion_claimed'])

    def test_explicit_first_overshoot_is_rejected_not_forced(self):
        value = after_shifted_window(shifted_window())
        chosen, _ = self.choose(value)
        self.assertEqual(chosen['delta'], -480)
        with self.assertRaisesRegex(ValueError, '^COLLECTION_SCROLL_CALIBRATION_NO_SAFE_PROFILE$') as failure:
            self.choose(value, first_delta=-600)
        self.assertEqual(failure.exception.candidate_rejections[0]['reason'],
                         'COLLECTION_SCROLL_BATCH_UNSAFE_DISTANCE')

    def test_second_and_later_windows_revert_to_normal_adaptive_choice(self):
        self.assertEqual(self.choose(first_delta=-600)[0]['delta'], -600)
        self.assertEqual(self.choose(first_delta=-600, index=1)[0]['delta'], -480)
        self.assertEqual(self.choose(shifted_window(), first_delta=-480, index=2)[0]['delta'], -600)

    def test_unmeasured_or_invalid_first_delta_and_window_index_fail(self):
        with self.assertRaisesRegex(ValueError, '^COLLECTION_SCROLL_TEST_FIRST_DELTA_UNMEASURED$'):
            self.choose(profiles=[calibration()], first_delta=-600)
        for value in (-120, 600, -600.0, True):
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, '^COLLECTION_SCROLL_TEST_FIRST_DELTA$'):
                self.choose(first_delta=value)
        for value in (-1, 0.0, True):
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, '^COLLECTION_SCROLL_TEST_WINDOW_INDEX$'):
                self.choose(index=value)

    def test_first_override_does_not_hide_stale_coverage_or_wrong_shape(self):
        value = packet()
        coverage = build_scroll_coverage(value, rule(), processed(value, 'read_only_observed'),
                                         mode='read_only_probe')
        coverage['frame_id'] = 'stale:fixture'
        with self.assertRaisesRegex(ValueError, '^COLLECTION_SCROLL_COVERAGE_EXPIRED$'):
            choose_window_profile(value, rule(), coverage, [five_notches()], window_index=0, first_delta=-600)
        profile = five_notches()
        profile['scrollbar']['thumb_height_range'] = [50, 52]
        with self.assertRaisesRegex(ValueError, '^COLLECTION_SCROLL_CALIBRATION_NO_SAFE_PROFILE$'):
            self.choose(value, [profile], first_delta=-600)


class ActualRecordedProfileTests(unittest.TestCase):
    def test_recorded_real_before_packet_supports_first_five_then_default_four_without_input(self):
        root = Path(__file__).resolve().parents[2]
        run = root / 'artifacts/collection_scroll_speed/probe_03'
        bank = root / 'artifacts/collection_scroll_speed/calibration_bank.json'
        if not bank.is_file() or not (run / 'session.json').is_file():
            self.skipTest('independent local measured profiles absent')
        profiles, _ = load_scroll_profile_bank(bank)
        record = json.loads((run / 'result.json').read_text('utf-8'))
        session = json.loads((run / 'session.json').read_text('utf-8'))
        before = next(step['result'] for step in session['steps'] if step.get('result', {})
                      .get('collection_observation', {}).get('frame_id') == record['before']['frame_id'])
        coverage = build_scroll_coverage(before, record['row'], record['readings'][:6], mode='read_only_probe')
        saved = copy.deepcopy((before, coverage, profiles))
        forced = choose_window_profile(before, record['row'], coverage, profiles, window_index=0, first_delta=-600)
        normal = choose_window_profile(before, record['row'], coverage, profiles, window_index=1, first_delta=-600)
        self.assertEqual(forced['delta'], -600)
        self.assertEqual(normal['delta'], -480)
        self.assertEqual((before, coverage, profiles), saved)
        plan = scroll_plan(before, record['row'], [], delta=-600, calibration=forced, coverage=coverage)
        self.assertEqual(validate_scroll_lease(before, record['row'], [], plan['steps'][0]['lease']),
                         plan['steps'][0]['lease'])


if __name__ == '__main__':
    unittest.main()
