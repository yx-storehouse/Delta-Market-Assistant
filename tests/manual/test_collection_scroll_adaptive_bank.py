"""Measured-profile selection contracts using synthetic, non-live packets."""
import unittest

from collection_scroll import (build_scroll_coverage, observe_layout, rebind_after_scroll,
    scroll_plan, select_window_scroll_calibration, validate_scroll_calibration, validate_scroll_lease)
from run_collection_trial import CollectionTrial
from test_collection_batch_scroll import calibration, processed
from test_collection_scroll import card, next_frame, packet, rule
from test_collection_trial_batch_scroll import (ContractSession, geometry_after_batch, geometry_profile,
    processed_window, row, snapshot)


def shifted_window(offset=50):
    result = packet()
    for member in result['collection_layout']['cards']:
        member['bounds'][1] += offset
        if member['fields_bounds'] is not None:
            member['fields_bounds'][1] += offset
        else:
            member['bounds'][3] -= offset
    return result


def five_notches():
    profile = calibration(-600)
    profile.update(content_displacement_pixels=[731, 739], thumb_displacement_pixels=[33, 37])
    return profile


def after_shifted_window(before, distance=735, thumb_shift=35):
    result = next_frame(before, digest='4', frame_id='synthetic:five-notch:after')
    listing = result['collection_layout']['listing_viewport']
    top, bottom = listing[1], listing[1]+listing[3]
    boundary = max(member['bounds'][1] for member in before['collection_layout']['cards']) - distance
    cards = []
    for column, x in enumerate((120, 997)):
        for index in range(-1, 4):
            raw_top = boundary + index*275
            raw_bottom = raw_top+266
            visible_top, visible_bottom = max(top, raw_top), min(bottom, raw_bottom)
            if visible_bottom <= visible_top:
                continue
            partial = visible_top != raw_top or visible_bottom != raw_bottom
            member = card(f'new-{column}-{index}', x, visible_top,
                          height=visible_bottom-visible_top, partial=partial)
            if partial:
                member['edges'].update(top=visible_top == raw_top, bottom=visible_bottom == raw_bottom)
            cards.append(member)
    result['collection_layout']['cards'] = cards
    result['collection_layout']['scrollbar']['thumb_bounds'][1] += thumb_shift
    return result


class AdaptiveCalibrationTests(unittest.TestCase):
    def choose(self, value, profiles):
        coverage = build_scroll_coverage(value, rule(), processed(value))
        return select_window_scroll_calibration(value, rule(), coverage, profiles)

    def test_four_notches_selected_when_shorter_measured_distance_clears_complete_rows(self):
        profiles = [five_notches(), calibration()]
        chosen = self.choose(packet(), profiles)
        self.assertEqual(chosen['delta'], -480)
        chosen['source']['record_path'] = 'mutated-copy'
        self.assertNotEqual(profiles[1]['source']['record_path'], 'mutated-copy')

    def test_five_notches_selected_when_four_would_leave_a_complete_scanned_row(self):
        before = shifted_window()
        profile = self.choose(before, [calibration(), five_notches()])
        self.assertEqual(profile['delta'], -600)
        coverage = build_scroll_coverage(before, rule(), processed(before))
        lease = scroll_plan(before, rule(), [], delta=-600, calibration=profile,
                            coverage=coverage)['steps'][0]['lease']
        self.assertEqual(lease['batch_bounds']['minimum_displacement_pixels'], 604)
        self.assertEqual(validate_scroll_lease(before, rule(), [], lease), lease)
        rebound = rebind_after_scroll(after_shifted_window(before), rule(), lease)
        self.assertEqual(rebound['batch_scroll_evidence']['displacement_interval_pixels'], [732, 738])
        self.assertFalse(rebound['batch_scroll_evidence']['skipped_by_old_identity'])
        self.assertFalse(rebound['old_card_coordinates_reused'])

    def test_selection_uses_actual_distance_not_wheel_notch_count(self):
        four = calibration()
        four['content_displacement_pixels'] = [731, 739]
        five = five_notches()
        five['content_displacement_pixels'] = [598, 602]
        self.assertEqual(self.choose(packet(), [four, five])['delta'], -600)

    def test_bank_never_scales_a_measurement_to_an_unmeasured_thumb_height(self):
        value = packet()
        value['collection_layout']['scrollbar']['thumb_bounds'][3] = 300
        with self.assertRaisesRegex(ValueError, '^COLLECTION_SCROLL_CALIBRATION_NO_SAFE_PROFILE$') as failure:
            self.choose(value, [calibration(), five_notches()])
        rejected = failure.exception.candidate_rejections
        self.assertEqual(len(rejected), 2)
        self.assertTrue(all(item['reason'] == 'COLLECTION_SCROLL_CALIBRATION_SCROLLBAR' for item in rejected))
        self.assertTrue(all(len(item['calibration_sha256']) == 64 for item in rejected))
        independently_measured = five_notches()
        independently_measured['scrollbar']['thumb_height_range'] = [299, 301]
        chosen = self.choose(value, [calibration(), independently_measured])
        self.assertEqual(chosen, independently_measured)

    def test_no_safe_candidate_reports_reasons_instead_of_manufacturing_single_notch(self):
        with self.assertRaisesRegex(ValueError, '^COLLECTION_SCROLL_CALIBRATION_NO_SAFE_PROFILE$') as failure:
            self.choose(shifted_window(), [calibration()])
        self.assertEqual(failure.exception.candidate_rejections[0]['reason'],
                         'COLLECTION_SCROLL_BATCH_UNSAFE_DISTANCE')

    def test_malformed_profile_or_expired_coverage_is_not_hidden_by_valid_alternative(self):
        invalid = calibration()
        invalid['content_displacement_pixels'] = [float('nan'), 592]
        with self.assertRaisesRegex(ValueError, '^COLLECTION_SCROLL_CALIBRATION_DISTANCE$'):
            self.choose(packet(), [invalid, calibration()])
        value = packet()
        coverage = build_scroll_coverage(value, rule(), processed(value))
        coverage['frame_id'] = 'stale:frame'
        with self.assertRaisesRegex(ValueError, '^COLLECTION_SCROLL_COVERAGE_EXPIRED$'):
            select_window_scroll_calibration(value, rule(), coverage, [calibration()])
        for profiles in ([], None, [calibration()]*65):
            with self.subTest(profiles=type(profiles).__name__), self.assertRaisesRegex(ValueError, 'CALIBRATION_BANK'):
                self.choose(value, profiles)

    def test_five_notch_readback_requires_its_own_thumb_distance(self):
        before = shifted_window()
        profile = five_notches()
        lease = scroll_plan(before, rule(), [], delta=-600, calibration=profile,
            coverage=build_scroll_coverage(before, rule(), processed(before)))['steps'][0]['lease']
        with self.assertRaisesRegex(ValueError, 'COLLECTION_SCROLL_BATCH_THUMB_DISTANCE'):
            rebind_after_scroll(after_shifted_window(before, thumb_shift=28), rule(), lease)
        self.assertEqual(validate_scroll_calibration(dict(profile, delta=600))['delta'], 600)

    def test_partial_observed_top_and_one_side_can_anchor_without_becoming_clickable(self):
        value = packet()
        for member in value['collection_layout']['cards'][-2:]:
            member['edges']['right'] = False
            member['bounds_basis'] = 'clipped_search_region'
        profile = self.choose(value, [calibration()])
        self.assertEqual(profile['delta'], -480)
        layout = observe_layout(value)
        self.assertTrue(all(not member['selectable'] for member in layout['cards'][-2:]))
        self.assertTrue(all(member['fields_bounds'] is None for member in layout['cards'][-2:]))
        self.assertFalse(value['collection_layout']['cards'][-1]['edges']['right'])

    def test_partial_unknown_boundary_unknown_both_sides_and_different_rows_fail(self):
        for mutation in ('no_top', 'no_sides', 'different_row', 'unanchored_side'):
            value = packet()
            member = value['collection_layout']['cards'][-1]
            if mutation == 'no_top':
                member['edges']['top'] = False
            elif mutation == 'no_sides':
                member['edges'].update(left=False, right=False)
                member['bounds_basis'] = 'clipped_search_region'
            elif mutation == 'different_row':
                member['bounds'][1] += 4
                member['bounds'][3] -= 4
            else:
                member['edges']['right'] = False
            with self.subTest(mutation=mutation), self.assertRaisesRegex(ValueError, '^COLLECTION_SCROLL_CALIBRATION_NO_SAFE_PROFILE$'):
                self.choose(value, [calibration()])


class AdaptiveRunnerTests(unittest.TestCase):
    def test_runner_passes_selected_five_notch_profile_to_real_lease_and_rebind(self):
        session = ContractSession(geometry_after_batch())
        profile = geometry_profile()
        profile['delta'] = -600
        insufficient = geometry_profile()
        insufficient['content_displacement_pixels'] = [540, 546]
        profiles = [insufficient, profile]
        events = []
        trial = CollectionTrial(session, snapshot(), 'artifacts/inert/input.json', emit=events.append,
                                scroll_profile_bank=profiles)
        profiles[1]['delta'] = -120
        self.assertEqual(trial.scroll_profile_bank[1]['delta'], -600)
        trial.scroll_window(row(), processed_window(session.previous))
        self.assertEqual(session.steps[0]['lease']['delta'], -600)
        self.assertEqual(trial.summary['listing_scroll_policy']['mode'], 'calibrated_adaptive_window')
        self.assertEqual(len(trial.summary['listing_scroll_policy']['bank_calibration_sha256']), 2)
        self.assertEqual(events[-1]['event'], 'listing_scroll_window_complete')

    def test_no_matching_bank_profile_dispatches_nothing_and_logs_rejections(self):
        session = ContractSession()
        profile = geometry_profile()
        profile['content_displacement_pixels'] = [840, 860]
        events = []
        trial = CollectionTrial(session, snapshot(), 'artifacts/inert/input.json', emit=events.append,
                                scroll_profile_bank=[profile])
        with self.assertRaisesRegex(ValueError, '^COLLECTION_SCROLL_CALIBRATION_NO_SAFE_PROFILE$'):
            trial.scroll_window(row(), processed_window(session.previous))
        self.assertEqual(session.steps, [])
        self.assertEqual(events[-1]['event'], 'listing_scroll_profile_selection_failed')
        self.assertEqual(events[-1]['candidate_rejections'][0]['reason'], 'COLLECTION_SCROLL_BATCH_UNSAFE_DISTANCE')
        self.assertFalse(events[-1]['automatic_single_notch_fallback'])

    def test_bank_and_single_or_upward_profile_fail_before_actions(self):
        for options, expected in ((dict(scroll_profile=geometry_profile(), scroll_profile_bank=[geometry_profile()]),
                                  'COLLECTION_SCROLL_PROFILE_AMBIGUOUS'),
                                 (dict(scroll_profile_bank=[dict(geometry_profile(), delta=600)]),
                                  'COLLECTION_SCROLL_PROFILE_DIRECTION'),
                                 (dict(scroll_profile_bank=[]), 'COLLECTION_SCROLL_CALIBRATION_BANK')):
            session = ContractSession()
            with self.subTest(expected=expected), self.assertRaisesRegex(ValueError, '^'+expected+'$'):
                CollectionTrial(session, snapshot(), 'artifacts/inert/input.json', **options)
            self.assertEqual(session.steps, [])


if __name__ == '__main__':
    unittest.main()
