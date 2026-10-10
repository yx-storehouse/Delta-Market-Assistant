"""A missing partial-card left edge is not a measured column-left coordinate."""
import copy
import hashlib
import json
from pathlib import Path
import unittest

from collection_scroll import (_profile_layout_shape, build_scroll_coverage, observe_layout,
    scroll_plan, select_window_scroll_calibration, validate_scroll_lease)
from run_collection_trial import load_scroll_profile_bank
from test_collection_batch_scroll import calibration, processed
from test_collection_scroll import packet, rule
from verify_collection_scroll_live import choose_window_profile


def missing_partial_left(*, right_shift=0):
    value = packet()
    member = value['collection_layout']['cards'][-1]
    member['bounds'][0] = 991
    member['bounds'][2] = 871+right_shift
    member['edges']['left'] = False
    member['bounds_basis'] = 'clipped_search_region'
    return value


class PartialObservedRightColumnTests(unittest.TestCase):
    def choose(self, value):
        coverage = build_scroll_coverage(value, rule(), processed(value), mode='collection_completed')
        return select_window_scroll_calibration(value, rule(), coverage, [calibration()])

    def test_observed_right_binds_unique_validated_column_without_repairing_missing_left(self):
        value = missing_partial_left()
        before = copy.deepcopy(value)
        chosen = self.choose(value)
        self.assertEqual(chosen, calibration())
        self.assertEqual(value, before)
        member = observe_layout(value)['cards'][-1]
        self.assertFalse(member['selectable'])
        self.assertFalse(member['edges']['left'])
        self.assertEqual(member['bounds'][0], 991)

    def test_original_eight_pixel_horizontal_limit_is_inclusive_and_not_widened(self):
        self.assertEqual(self.choose(missing_partial_left(right_shift=-8))['delta'], -480)
        with self.assertRaisesRegex(ValueError, '^COLLECTION_SCROLL_CALIBRATION_NO_SAFE_PROFILE$'):
            self.choose(missing_partial_left(right_shift=-9))

    def test_no_observed_side_wrong_column_and_wrong_basis_are_not_anchors(self):
        for change in ('no_side', 'wrong_column', 'wrong_basis'):
            value = missing_partial_left()
            member = value['collection_layout']['cards'][-1]
            if change == 'no_side':
                member['edges']['right'] = False
            elif change == 'wrong_column':
                member['bounds'][2] = 858
            else:
                member['bounds_basis'] = 'observed'
            with self.subTest(change=change), self.assertRaisesRegex(ValueError, '^COLLECTION_SCROLL_CALIBRATION_NO_SAFE_PROFILE$'):
                self.choose(value)

    def test_full_card_left_outside_profile_and_full_card_missing_side_do_not_use_partial_exception(self):
        for change in ('wrong_left', 'missing_side'):
            value = packet()
            member = value['collection_layout']['cards'][1]
            if change == 'wrong_left':
                member['bounds'][0] -= 6
                member['fields_bounds'][0] -= 6
            else:
                member['edges']['left'] = False
            with self.subTest(change=change), self.assertRaisesRegex(ValueError, '^COLLECTION_SCROLL_CALIBRATION_NO_SAFE_PROFILE$'):
                self.choose(value)

    def test_partial_width_interval_remains_required(self):
        value = missing_partial_left()
        member = value['collection_layout']['cards'][-1]
        member['bounds'][0] -= 5
        member['bounds'][2] += 5
        with self.assertRaisesRegex(ValueError, '^COLLECTION_SCROLL_CALIBRATION_NO_SAFE_PROFILE$'):
            self.choose(value)

    def test_two_possible_observed_right_columns_are_rejected_defensively(self):
        # Direct helper-level invalid normalized layout. The public observer
        # rejects its overlapping columns even earlier; ambiguity must still
        # not be resolved by arbitrarily choosing the first anchor.
        layout = observe_layout(missing_partial_left())
        profile = calibration()
        profile['shape']['column_left_ranges'] = [[119, 121], [124, 126]]
        for member in layout['cards']:
            if member['selectable'] and member['bounds'][0] > 900:
                member['bounds'][0], member['bounds'][2] = 125, 860
        layout['cards'][-1]['bounds'][0] = 114
        with self.assertRaisesRegex(ValueError, '^COLLECTION_SCROLL_CALIBRATION_SHAPE$'):
            _profile_layout_shape(layout, profile)

    def test_all_full_anchor_shapes_are_validated_before_partial_association(self):
        value = missing_partial_left()
        value['collection_layout']['cards'][1]['bounds'][3] += 8
        with self.assertRaisesRegex(ValueError, '^COLLECTION_SCROLL_CALIBRATION_NO_SAFE_PROFILE$'):
            self.choose(value)


class RecordedWindow02ShapeRegression(unittest.TestCase):
    def test_actual_failed_packet_now_plans_measured_600_without_profile_mutation_or_input(self):
        root = Path(__file__).resolve().parents[2]
        run = root / 'artifacts/collection_scroll_speed/windows_02'
        bank_path = root / 'artifacts/collection_scroll_speed/calibration_bank.json'
        if not (run / 'result.json').is_file() or not bank_path.is_file():
            self.skipTest('local recorded window-02 packet absent')
        profiles, sources = load_scroll_profile_bank(bank_path)
        paths = [bank_path]+[Path(item['path']) for item in sources['profiles']]
        hashes = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in paths}
        record = json.loads((run / 'result.json').read_text('utf-8'))
        session = json.loads((run / 'session.json').read_text('utf-8'))
        snapshot = json.loads((root / 'artifacts/collection_scroll_speed/input/task_snapshot.json').read_text('utf-8'))
        current_rule = next(member for member in snapshot['rows'] if member['row_index']==7)
        before = next(step['result'] for step in reversed(session['steps']) if step.get('result'))
        layout = observe_layout(before)
        special = next(member for member in layout['cards'] if member['id']=='edge:991:1129:1858:1202')
        self.assertEqual(special['bounds'], [991,1129,868,74])
        self.assertFalse(special['edges']['left'])
        self.assertTrue(special['edges']['right'])
        self.assertFalse(special['selectable'])
        coverage = build_scroll_coverage(before, current_rule, record['windows'][0]['readings'], mode='read_only_probe')
        chosen = choose_window_profile(before, current_rule, coverage, profiles, window_index=0, first_delta=-600)
        lease = scroll_plan(before,current_rule,[],delta=-600,calibration=chosen,coverage=coverage)['steps'][0]['lease']
        self.assertEqual(validate_scroll_lease(before,current_rule,[],lease),lease)
        self.assertEqual(lease['batch_bounds']['minimum_displacement_pixels'],559)
        self.assertEqual(lease['batch_bounds']['maximum_displacement_pixels'],825)
        self.assertEqual(hashes,{str(path):hashlib.sha256(path.read_bytes()).hexdigest() for path in paths})


if __name__=='__main__':
    unittest.main()
