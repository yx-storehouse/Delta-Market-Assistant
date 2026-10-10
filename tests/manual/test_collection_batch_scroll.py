"""Synthetic bounded batch-wheel contracts; these are not live calibration."""
import copy
import json
from pathlib import Path
import unittest

from collection_candidate import match_selected_first_card
from collection_scroll import (build_scroll_coverage, observe_layout, rebind_after_scroll,
    scroll_plan, validate_scroll_calibration, validate_scroll_lease)
from test_collection_scroll import packet, rule, card, next_frame, record


def calibration(delta=-480):
    return dict(schema='collection-scroll-calibration-v1', delta=delta,
        source=dict(record_path='offline_synthetic_fixture.json', record_sha256='e'*64,
                    before_frame_id='synthetic:calibration:before', after_frame_id='synthetic:calibration:after',
                    measurement_method='offline_contract_fixture_not_live_evidence'),
        viewport=[2560, 1440], listing_viewport=[120, 303, 1742, 947],
        shape=dict(column_left_ranges=[[116, 125], [993, 1005]], card_width_range=[858, 874],
                   card_height_range=[258, 270], fields_height_range=[40, 46], row_pitch_range=[270, 280]),
        scrollbar=dict(track_bounds=[1870, 303, 8, 947], thumb_height_range=[199, 201]),
        content_displacement_pixels=[584, 592], thumb_displacement_pixels=[27, 30],
        content_pixels_per_thumb_pixel=[20, 22])


def processed(value=None, disposition='favorite_preserved'):
    value = value or packet()
    result = []
    for index, member in enumerate(observe_layout(value)['cards']):
        if not member['selectable']:
            continue
        candidate = dict(product=rule()['product_name'], condition=rule()['condition_label'],
            wear='0.' + str(100000+index), price='200', row_index=rule()['row_index'], eligible=True,
            source_frame_id='synthetic:processed:' + member['id'], source_frame_sha256='a'*64,
            card_id=member['id'], card_bounds=member['bounds'], fields_bounds=member['fields_bounds'],
            geometry_mode='observed_dynamic', purchase_phase_enabled=False)
        result.append(dict(card=copy.deepcopy(member), candidate=candidate, disposition=disposition))
    return result


def batch_after(before=None, *, distance=588, thumb_shift=28):
    result = next_frame(before or packet(), frame_id='synthetic:batch:after')
    layout = result['collection_layout']
    first_new = 1128-distance
    prior_top = 853-distance
    prior_bottom = prior_top+266
    top = layout['listing_viewport'][1]
    bottom = top+layout['listing_viewport'][3]
    cards = []
    for column, x in enumerate((120, 997)):
        clipped = card('old-clipped-' + str(column), x, top, height=prior_bottom-top, partial=True)
        clipped['edges'].update(top=False, bottom=True)
        cards += [clipped, card('new-first-' + str(column), x, first_new),
                  card('new-second-' + str(column), x, first_new+275),
                  card('new-partial-' + str(column), x, first_new+550,
                       height=bottom-first_new-550, partial=True)]
    layout['cards'] = cards
    layout['scrollbar']['thumb_bounds'][1] += thumb_shift
    return result


class BatchCoverageTests(unittest.TestCase):
    def test_all_current_complete_cards_must_have_unique_processed_observations(self):
        value = packet()
        items = processed(value)
        coverage = build_scroll_coverage(value, rule(), items)
        self.assertEqual(len(coverage['current_cards']), 6)
        self.assertTrue(coverage['collection_completion_claimed'])
        self.assertFalse(coverage['cross_scroll_identity_reuse'])
        self.assertEqual(coverage['frame_id'], value['collection_observation']['frame_id'])
        coverage['processed_cards'][0]['candidate']['price'] = '999'
        self.assertEqual(items[0]['candidate']['price'], '200')

    def test_missing_duplicate_and_wrong_geometry_do_not_claim_coverage(self):
        variants = [processed()[:-1], processed() + processed()[:1]]
        duplicate = processed()
        duplicate[-1] = copy.deepcopy(duplicate[0])
        variants.append(duplicate)
        moved = processed()
        moved[-1]['candidate']['card_bounds'] = [400, 500, 865, 266]
        variants.append(moved)
        for items in variants:
            with self.subTest(items=items), self.assertRaisesRegex(ValueError, 'COLLECTION_SCROLL_COVERAGE'):
                build_scroll_coverage(packet(), rule(), items)

    def test_candidate_identity_and_completion_disposition_remain_independent(self):
        for key, value in (('product', 'different product'), ('condition', '成色S'),
                           ('row_index', 7), ('source_frame_sha256', 'not-sha'),
                           ('geometry_mode', 'receipt_only_observed_dynamic'), ('price', '200.5')):
            items = processed()
            items[0]['candidate'][key] = value
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, 'COLLECTION_SCROLL_COVERAGE_CANDIDATE'):
                build_scroll_coverage(packet(), rule(), items)
        outside = processed()
        outside[0]['disposition'] = 'candidate_outside_rule'
        with self.assertRaisesRegex(ValueError, 'COLLECTION_SCROLL_COVERAGE_DISPOSITION'):
            build_scroll_coverage(packet(), rule(), outside)

    def test_read_only_probe_is_explicit_not_collection_completion(self):
        items = processed(disposition='read_only_observed')
        with self.assertRaisesRegex(ValueError, 'COLLECTION_SCROLL_COVERAGE_DISPOSITION'):
            build_scroll_coverage(packet(), rule(), items)
        coverage = build_scroll_coverage(packet(), rule(), items, mode='read_only_probe')
        self.assertFalse(coverage['collection_completion_claimed'])
        self.assertEqual(coverage['mode'], 'read_only_probe')


class BatchScrollTests(unittest.TestCase):
    def token(self, value=None, profile=None, coverage=None):
        value = value or packet()
        profile = profile or calibration()
        if coverage is None:
            coverage = build_scroll_coverage(value, rule(), processed(value))
        return scroll_plan(value, rule(), [], delta=profile['delta'], calibration=profile,
                           coverage=coverage)['steps'][0]['lease']

    def test_profile_is_explicit_and_deep_frozen(self):
        original = calibration()
        frozen = validate_scroll_calibration(original)
        frozen['shape']['column_left_ranges'][0][0] += 1
        self.assertNotEqual(frozen, original)
        with self.assertRaisesRegex(ValueError, 'COLLECTION_SCROLL_CALIBRATION'):
            scroll_plan(packet(), rule(), [], delta=-480)
        with self.assertRaisesRegex(ValueError, 'COLLECTION_SCROLL_CALIBRATION_DELTA'):
            scroll_plan(packet(), rule(), [], delta=-120, calibration=original)

    def test_batch_removes_scanned_full_rows_without_skipping_bottom_unscanned_row(self):
        value = packet()
        token = self.token(value)
        bounds = token['batch_bounds']
        self.assertEqual(bounds['minimum_displacement_pixels'], 554)
        self.assertEqual(bounds['maximum_displacement_pixels'], 821)
        self.assertEqual(validate_scroll_lease(value, rule(), [], token), token)
        rebound = rebind_after_scroll(batch_after(value), rule(), token)
        batch = rebound['batch_scroll_evidence']
        self.assertEqual(batch['displacement_interval_pixels'], [585, 591])
        self.assertTrue(batch['old_complete_exit_under_calibrated_motion_model'])
        self.assertTrue(batch['first_unscanned_boundary_visible_under_calibrated_motion_model'])
        self.assertFalse(batch['item_identity_proven'])
        self.assertFalse(batch['skipped_by_old_identity'])
        self.assertTrue(rebound['content_membership_not_proven'])
        self.assertFalse(rebound['collection_allowed'])
        self.assertFalse(rebound['page_exhausted'])
        self.assertEqual(len(rebound['selectable_card_ids']), 4)

    def test_single_notch_distance_is_not_claimed_to_clear_scanned_rows(self):
        profile = calibration()
        profile['content_displacement_pixels'] = [145, 149]
        with self.assertRaisesRegex(ValueError, 'COLLECTION_SCROLL_BATCH_UNSAFE_DISTANCE'):
            self.token(profile=profile)

    def test_whole_viewport_jump_cannot_skip_observed_partial_row(self):
        profile = calibration()
        profile['content_displacement_pixels'] = [900, 905]
        with self.assertRaisesRegex(ValueError, 'COLLECTION_SCROLL_BATCH_UNSAFE_DISTANCE'):
            self.token(profile=profile)

    def test_profile_without_source_or_actual_batch_delta_is_rejected(self):
        for mutate in ('source', 'delta', 'frames', 'wide_distance', 'nan'):
            profile = calibration()
            if mutate == 'source':
                profile['source']['record_sha256'] = 'missing'
            elif mutate == 'delta':
                profile['delta'] = -120
            elif mutate == 'frames':
                profile['source']['after_frame_id'] = profile['source']['before_frame_id']
            elif mutate == 'wide_distance':
                profile['content_displacement_pixels'] = [500, 800]
            else:
                profile['content_pixels_per_thumb_pixel'][0] = float('nan')
            with self.subTest(mutate=mutate), self.assertRaisesRegex(ValueError, 'COLLECTION_SCROLL_CALIBRATION'):
                validate_scroll_calibration(profile)

    def test_viewport_card_shape_pitch_and_thumb_shape_are_required(self):
        for mutate in ('viewport', 'width', 'height', 'pitch', 'thumb'):
            profile = calibration()
            if mutate == 'viewport':
                profile['listing_viewport'][3] -= 1
            elif mutate == 'width':
                profile['shape']['card_width_range'] = [800, 820]
            elif mutate == 'height':
                profile['shape']['card_height_range'] = [280, 300]
            elif mutate == 'pitch':
                profile['shape']['row_pitch_range'] = [290, 310]
            else:
                profile['scrollbar']['thumb_height_range'] = [100, 120]
            with self.subTest(mutate=mutate), self.assertRaisesRegex(ValueError, 'COLLECTION_SCROLL_CALIBRATION'):
                self.token(profile=profile)

    def test_unscanned_boundary_must_be_observed_for_both_columns(self):
        for mutate in ('missing', 'unknown_top', 'unknown_side'):
            value = packet()
            if mutate == 'missing':
                value['collection_layout']['cards'].pop()
            elif mutate == 'unknown_top':
                value['collection_layout']['cards'][-1]['edges']['top'] = False
            else:
                value['collection_layout']['cards'][-1]['edges']['right'] = False
            with self.subTest(mutate=mutate), self.assertRaisesRegex(ValueError, 'UNSCANNED_BOUNDARY_UNPROVEN'):
                self.token(value)

    def test_lease_revalidation_rejects_old_coverage_and_modified_point(self):
        value = packet()
        token = self.token(value)
        with self.assertRaisesRegex(ValueError, 'COLLECTION_SCROLL_COVERAGE_EXPIRED'):
            validate_scroll_lease(next_frame(value), rule(), [], token)
        modified = copy.deepcopy(token)
        modified['point'] = [2339, 320]
        with self.assertRaisesRegex(ValueError, 'COLLECTION_SCROLL_LEASE_EXPIRED'):
            validate_scroll_lease(value, rule(), [], modified)
        modified = copy.deepcopy(token)
        modified['batch_bounds']['maximum_displacement_pixels'] += 100
        with self.assertRaisesRegex(ValueError, 'COLLECTION_SCROLL_LEASE_EXPIRED'):
            validate_scroll_lease(value, rule(), [], modified)

    def test_pending_journal_blocks_batch_as_it_blocks_single_notch(self):
        value = packet()
        for status in ('prepared', 'dispatched', 'input_uncertain'):
            with self.subTest(status=status), self.assertRaisesRegex(ValueError, 'COLLECTION_PENDING_RECONCILIATION'):
                scroll_plan(value, rule(), [record(status)], delta=-480, calibration=calibration(),
                    coverage=build_scroll_coverage(value, rule(), processed(value)))

    def test_wrong_thumb_distance_or_direction_rejected_before_any_candidate(self):
        for shift, expected in ((7, 'BATCH_THUMB_DISTANCE'), (36, 'BATCH_THUMB_DISTANCE')):
            with self.subTest(shift=shift), self.assertRaisesRegex(ValueError, expected):
                rebind_after_scroll(batch_after(thumb_shift=shift), rule(), self.token())
        value = packet()
        value['collection_layout']['scrollbar']['thumb_bounds'][1] = 400
        with self.assertRaisesRegex(ValueError, 'DIRECTION_CONFLICT'):
            rebind_after_scroll(batch_after(value, thumb_shift=-28), rule(), self.token(value))

    def test_unchanged_thumb_and_height_change_over_one_remain_rejected(self):
        with self.assertRaisesRegex(ValueError, 'PROGRESS_UNPROVEN'):
            rebind_after_scroll(batch_after(thumb_shift=0), rule(), self.token())
        after = batch_after()
        after['collection_layout']['scrollbar']['thumb_bounds'][3] += 2
        with self.assertRaisesRegex(ValueError, 'CONTENT_CHANGED'):
            rebind_after_scroll(after, rule(), self.token())

    def test_next_complete_boundary_row_must_be_uniquely_observed(self):
        after = batch_after()
        after['collection_layout']['cards'] = [card for card in after['collection_layout']['cards']
                                               if not card['id'].startswith('new-first-')]
        with self.assertRaisesRegex(ValueError, 'COLLECTION_SCROLL_BATCH_BOUNDARY'):
            rebind_after_scroll(after, rule(), self.token())

    def test_distance_model_must_agree_with_independent_thumb_motion(self):
        profile = calibration()
        profile['content_pixels_per_thumb_pixel'] = [5, 6]
        with self.assertRaisesRegex(ValueError, 'COLLECTION_SCROLL_BATCH_BOUNDARY'):
            rebind_after_scroll(batch_after(), rule(), self.token(profile=profile))

    def test_same_image_same_frame_and_modified_profile_still_rejected(self):
        before = packet()
        with self.assertRaisesRegex(ValueError, 'REOBSERVATION_REQUIRED'):
            rebind_after_scroll(before, rule(), self.token(before))
        changed = self.token(before)
        changed['calibration']['content_displacement_pixels'] = [585, 593]
        with self.assertRaisesRegex(ValueError, 'CALIBRATION_BINDING'):
            rebind_after_scroll(batch_after(), rule(), changed)

    def test_explicit_up_batch_preserves_unscanned_top_boundary(self):
        before = batch_after()
        profile = calibration(delta=480)
        token = self.token(before, profile)
        after = next_frame(packet(), digest='3', frame_id='synthetic:up:after')
        rebound = rebind_after_scroll(after, rule(), token)
        self.assertEqual(rebound['scrollbar_shift_pixels'], -28)
        self.assertEqual(token['batch_bounds']['direction'], 'up')
        self.assertEqual(token['batch_bounds']['minimum_displacement_pixels'], 448)
        self.assertEqual(token['batch_bounds']['maximum_displacement_pixels'], 715)
        self.assertEqual(rebound['batch_scroll_evidence']['displacement_interval_pixels'], [585, 591])
        self.assertFalse(rebound['batch_scroll_evidence']['item_identity_proven'])


class RecordedSingleNotchMeasurements(unittest.TestCase):
    def test_actual_selected_identity_proves_small_motion_not_a_new_page(self):
        root = Path(__file__).resolve().parents[2]
        source = root / 'artifacts/collection_acceptance/input/task_snapshot.json'
        if not source.is_file():
            self.skipTest('local recorded configuration absent')
        rows = json.loads(source.read_text(encoding='utf-8'))['rows']
        checks = [('run_03', 166, 146, '4.594406'), ('run_03', 180, 147, '4.594406'),
                  ('run_03', 196, 147, '3.529880'), ('run_04', 33, 146, '3.529880'),
                  ('run_04', 47, 147, '3.529880')]
        for run, index, distance, wear in checks:
            path = root / 'artifacts/collection_speed' / run / 'session.json'
            if not path.is_file():
                self.skipTest('local recorded single-notch sequence absent')
            steps = json.loads(path.read_text(encoding='utf-8'))['steps']
            step = steps[index]
            self.assertEqual(step['kind'], 'scroll_visible_list')
            lease = step['geometry_action']['lease']
            self.assertEqual(lease['delta'], -120)
            before = next(item['result'] for item in reversed(steps[:index]) if item.get('result'))
            after = next(item['result'] for item in steps[index+1:] if item.get('result'))
            actual_rule = next(row for row in rows if row['row_index'] == lease['scope']['row_index'])
            old = match_selected_first_card(before, actual_rule)
            new = match_selected_first_card(after, actual_rule)
            with self.subTest(run=run, index=index):
                for key in ('product', 'condition', 'wear', 'price'):
                    self.assertEqual(old[key], new[key])
                self.assertEqual(old['wear'], wear)
                self.assertEqual(old['card_bounds'][1]-new['card_bounds'][1], distance)
                self.assertIn(old['fields_bounds'][1]-new['fields_bounds'][1], range(144, 148))
                # No measured -480 profile is manufactured from these five
                # -120 observations. Batch still requires its own source.
                with self.assertRaisesRegex(ValueError, 'COLLECTION_SCROLL_CALIBRATION'):
                    scroll_plan(before, actual_rule, [], delta=-480)


if __name__ == '__main__':
    unittest.main()
