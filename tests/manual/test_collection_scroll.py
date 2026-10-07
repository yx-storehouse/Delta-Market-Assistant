"""Offline geometry-contract tests, NOT evidence of live scroll calibration.

The first fixture's rectangles reproduce the historical six-card coordinate
observations; later scroll rectangles/scrollbar readings are synthetic contract
cases. No frame images, game processes or OS input are read/written here.
"""

import copy
import json
from pathlib import Path
import unittest

from collection_journal import candidate_key
from collection_scroll import (
    bind_card, collection_disposition, observe_layout, rebind_after_scroll,
    rebind_selected, scroll_plan, selection_plan, validate_card_lease,
    validate_scroll_lease,
)


ROOT = Path(__file__).resolve().parents[2]


def rule():
    return dict(enabled=True, dictionary_resolved=True, product_name='P90冲锋枪-天命',
                condition_label='成色B', row_index=6)


def candidate():
    # Exact selected fields recorded by live_series_s6_v5/live_5.json.
    return dict(product='P90冲锋枪-天命', condition='成色B', wear='2.198642', price='230')


def record(status='confirmed', item=None):
    item = item or candidate()
    return dict(key=candidate_key(item), status=status, candidate=item)


def card(card_id, x, y, *, height=266, partial=False, selected=False):
    return dict(id=card_id, bounds=[x, y, 865, height],
                edges=dict(top=True, bottom=not partial, left=True, right=True),
                fields_bounds=None if partial else [x, y + 222, 865, 44], selected=selected)


def packet(digest='1', frame_id='fixture:before'):
    # The observed 877 px column pitch/275 px row pitch are fixture data, not
    # formulas used by the production planner. The viewport's bottom and all
    # scrollbar measurements are intentionally synthetic test cases.
    cards = [card(f'c{row * 2 + col}', 120 + col * 877, 303 + row * 275)
             for row in range(3) for col in range(2)]
    cards += [card('partial-l', 120, 1128, height=122, partial=True),
              card('partial-r', 997, 1128, height=122, partial=True)]
    return dict(startup_page=dict(page='skin_listings', overlay='none'),
                frames=[dict(width=2560, height=1440, sha256=digest * 64)],
                collection_observation=dict(same_frame=True, frame_id=frame_id,
                    frame_sha256=digest * 64, regions=[dict(kind='product_title', ok=True,
                        truncated=False, words=[dict(text='P90冲锋枪-天命', x=1972, y=244,
                                                    width=210, height=30)])]),
                collection_layout=dict(schema='collection-layout-v1', detector='visible_edges',
                    complete=True, same_frame=True, frame_id=frame_id, frame_sha256=digest * 64,
                    viewport=[2560, 1440], listing_viewport=[120, 303, 1742, 947], cards=cards,
                    scrollbar=dict(track_bounds=[1870, 303, 8, 947],
                                   thumb_bounds=[1870, 303, 8, 200])))


def next_frame(value, digest='2', frame_id='fixture:after'):
    result = copy.deepcopy(value)
    result['frames'][-1]['sha256'] = digest * 64
    for key in ('collection_layout', 'collection_observation'):
        result[key]['frame_id'] = frame_id
        result[key]['frame_sha256'] = digest * 64
    return result


def scrolled(value):
    result = next_frame(value)
    layout = result['collection_layout']
    # Deliberately non-grid translation proves the consumer uses new observed
    # rectangles, not old row-major indices or multiples of a fixed row size.
    layout['cards'] = [card('new-l', 120, 361), card('new-r', 997, 361),
                       card('new-bottom-l', 120, 636), card('new-bottom-r', 997, 636)]
    layout['scrollbar']['thumb_bounds'][1] += 40
    return result


class LayoutTests(unittest.TestCase):
    def test_full_and_partial_cards_are_distinct(self):
        layout = observe_layout(packet())
        self.assertEqual(len([c for c in layout['cards'] if c['selectable']]), 6)
        self.assertEqual(layout['partial_card_ids'], ['partial-l', 'partial-r'])
        self.assertFalse(layout['page_exhausted'])

    def test_empty_visible_list_is_not_page_exhaustion(self):
        value = packet()
        value['collection_layout']['cards'] = []
        self.assertFalse(observe_layout(value)['page_exhausted'])

    def test_legacy_fixed_six_card_packet_cannot_be_upgraded(self):
        value = packet()
        del value['collection_layout']
        value['collection_selected_card'] = dict(index=5, selected=True)
        with self.assertRaisesRegex(ValueError, 'COLLECTION_LAYOUT_MISSING'):
            observe_layout(value)

    def test_stale_projection_and_layout_rejected(self):
        for section in ('collection_observation', 'collection_layout'):
            value = packet()
            value[section]['frame_sha256'] = 'f' * 64
            with self.subTest(section=section), self.assertRaises(ValueError):
                observe_layout(value)
        value = packet()
        value['collection_layout']['frame_id'] = 'other'
        with self.assertRaisesRegex(ValueError, 'COLLECTION_LAYOUT_FRAME_BINDING'):
            observe_layout(value)

    def test_unknown_or_incomplete_detector_rejected(self):
        for key, new_value in [('schema', 'legacy'), ('detector', 'fixed_index'),
                               ('complete', False), ('complete', 1), ('same_frame', False)]:
            value = packet()
            value['collection_layout'][key] = new_value
            with self.subTest(key=key), self.assertRaises(ValueError):
                observe_layout(value)

    def test_wrong_page_or_overlay_rejected(self):
        for key, new_value in [('page', 'watchlist_listings'), ('overlay', 'listing_filter')]:
            value = packet()
            value['startup_page'][key] = new_value
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, 'COLLECTION_PAGE'):
                observe_layout(value)

    def test_outside_viewport_or_field_rect_rejected(self):
        for target, bounds in [('bounds', [0, 0, 865, 266]),
                               ('fields_bounds', [120, 1300, 865, 44]),
                               ('bounds', [120, 303, 0, 266]),
                               ('bounds', [120, 303, 865.0, 266]),
                               ('bounds', [True, 303, 865, 266])]:
            value = packet()
            value['collection_layout']['cards'][0][target] = bounds
            with self.subTest(target=target, bounds=bounds), self.assertRaises(ValueError):
                observe_layout(value)

    def test_duplicate_ids_and_overlap_rejected(self):
        for key in ('id', 'bounds'):
            value = packet()
            value['collection_layout']['cards'][1][key] = value['collection_layout']['cards'][0][key]
            with self.subTest(key=key), self.assertRaises(ValueError):
                observe_layout(value)

    def test_edges_and_selection_are_observed_booleans(self):
        for mutate in (lambda c: c['edges'].update(bottom='true'),
                       lambda c: c['edges'].pop('top'),
                       lambda c: c.update(selected=1)):
            value = packet()
            mutate(value['collection_layout']['cards'][0])
            with self.assertRaises(ValueError):
                observe_layout(value)
        value = packet()
        for item in value['collection_layout']['cards'][:2]:
            item['selected'] = True
        with self.assertRaisesRegex(ValueError, 'COLLECTION_LAYOUT_SELECTION_AMBIGUOUS'):
            observe_layout(value)

    def test_missing_field_strip_is_not_selectable(self):
        value = packet()
        value['collection_layout']['cards'][0]['fields_bounds'] = None
        with self.assertRaisesRegex(ValueError, 'COLLECTION_PARTIAL_CARD_REQUIRES_SCROLL'):
            bind_card(value, rule(), 'c0')

    def test_geometry_and_budget_boundaries(self):
        value = packet()
        value['collection_layout']['cards'] *= 5
        with self.assertRaisesRegex(ValueError, 'COLLECTION_LAYOUT_CARD_COUNT'):
            observe_layout(value)
        for size in (0, 20000, True):
            value = packet()
            value['frames'][0]['width'] = size
            with self.assertRaises(ValueError):
                observe_layout(value)

    def test_input_not_mutated_and_return_copy(self):
        value = packet()
        before = copy.deepcopy(value)
        layout = observe_layout(value)
        layout['cards'][0]['bounds'][0] = 999
        self.assertEqual(value, before)


class SelectionTests(unittest.TestCase):
    def test_point_comes_from_current_observed_body(self):
        value = scrolled(packet())
        lease = bind_card(value, rule(), 'new-l')
        self.assertEqual(lease['point'], [552, 472])
        self.assertNotEqual(lease['point'], [500, 420])
        self.assertEqual(validate_card_lease(value, rule(), lease), lease)

    def test_half_card_has_no_selection_point(self):
        with self.assertRaisesRegex(ValueError, 'COLLECTION_PARTIAL_CARD_REQUIRES_SCROLL'):
            bind_card(packet(), rule(), 'partial-l')

    def test_wrong_title_and_disabled_rule_stop(self):
        for key, new_value in [('product_name', 'AUG突击步枪-天命'), ('enabled', False),
                               ('row_index', True), ('condition_label', '未知')]:
            new_rule = rule()
            new_rule[key] = new_value
            with self.subTest(key=key), self.assertRaises(ValueError):
                bind_card(packet(), new_rule, 'c0')

    def test_card_lease_expires_on_new_capture_even_without_motion(self):
        value = packet()
        lease = bind_card(value, rule(), 'c0')
        with self.assertRaisesRegex(ValueError, 'COLLECTION_CARD_LEASE_EXPIRED'):
            validate_card_lease(next_frame(value), rule(), lease)

    def test_point_tampering_rejected(self):
        value = packet()
        lease = bind_card(value, rule(), 'c0')
        lease['point'] = [2339, 320]
        with self.assertRaisesRegex(ValueError, 'COLLECTION_CARD_LEASE_EXPIRED'):
            validate_card_lease(value, rule(), lease)

    def test_selection_requires_new_frame_and_observed_selected_geometry(self):
        before = packet()
        lease = bind_card(before, rule(), 'c0')
        with self.assertRaisesRegex(ValueError, 'COLLECTION_REOBSERVATION_REQUIRED'):
            rebind_selected(before, rule(), lease)
        after = next_frame(before)
        with self.assertRaisesRegex(ValueError, 'COLLECTION_SELECTION_GEOMETRY_CHANGED'):
            rebind_selected(after, rule(), lease)
        after['collection_layout']['cards'][0].update(selected=True, id='new-frame-id')
        result = rebind_selected(after, rule(), lease)
        self.assertTrue(result['selected_observed'])
        self.assertFalse(result['collection_allowed'])
        self.assertFalse(result['item_identity_proven'])
        self.assertEqual(result['lease']['card']['id'], 'new-frame-id')
        validate_card_lease(after, rule(), result['lease'])

    def test_unexpected_scroll_between_select_and_readback_stops(self):
        before = packet()
        lease = bind_card(before, rule(), 'c0')
        after = scrolled(before)
        after['collection_layout']['cards'][0]['selected'] = True
        with self.assertRaisesRegex(ValueError, 'COLLECTION_SELECTION_GEOMETRY_CHANGED'):
            rebind_selected(after, rule(), lease)

    def test_plan_contains_only_selection_and_readback(self):
        plan = selection_plan(packet(), rule(), 'c0')
        self.assertEqual([s['kind'] for s in plan['steps']], ['select_visible_card', 'capture'])
        self.assertTrue(plan['steps'][-1]['collection_layout'])
        self.assertFalse(plan['purchase_phase_enabled'])


class ScrollTests(unittest.TestCase):
    def token(self, value=None):
        return scroll_plan(value or packet(), rule(), [])['steps'][0]['lease']

    def test_scroll_requires_same_frame_scrollbar_evidence(self):
        value = packet()
        del value['collection_layout']['scrollbar']
        with self.assertRaisesRegex(ValueError, 'COLLECTION_SCROLL_PROGRESS_UNPROVEN'):
            scroll_plan(value, rule(), [])

    def test_one_notch_only_and_no_mutation_in_plan(self):
        for delta in (-360, -240, 0, 240, True, -120.0):
            with self.subTest(delta=delta), self.assertRaisesRegex(ValueError, 'COLLECTION_SCROLL_BOUND'):
                scroll_plan(packet(), rule(), [], delta=delta)
        plan = scroll_plan(packet(), rule(), [])
        self.assertEqual([s['kind'] for s in plan['steps']], ['scroll_visible_list', 'capture'])
        self.assertFalse(plan['page_exhausted'])

    def test_scroll_lease_expires_or_rejects_modified_point(self):
        value = packet()
        token = self.token(value)
        self.assertEqual(validate_scroll_lease(value, rule(), [], token), token)
        with self.assertRaisesRegex(ValueError, 'COLLECTION_SCROLL_LEASE_EXPIRED'):
            validate_scroll_lease(next_frame(value), rule(), [], token)
        token['point'] = [2339, 320]
        with self.assertRaisesRegex(ValueError, 'COLLECTION_SCROLL_LEASE_EXPIRED'):
            validate_scroll_lease(value, rule(), [], token)

    def test_hash_change_alone_does_not_prove_scroll(self):
        before = packet()
        with self.assertRaisesRegex(ValueError, 'COLLECTION_SCROLL_PROGRESS_UNPROVEN'):
            rebind_after_scroll(next_frame(before), rule(), self.token(before))

    def test_scroll_result_rebinds_current_geometry_not_old_indices(self):
        before = packet()
        after = scrolled(before)
        result = rebind_after_scroll(after, rule(), self.token(before))
        self.assertEqual(result['scrollbar_shift_pixels'], 40)
        self.assertEqual(result['selectable_card_ids'], ['new-l', 'new-r', 'new-bottom-l', 'new-bottom-r'])
        self.assertTrue(result['requires_selected_reobservation'])
        self.assertFalse(result['page_exhausted'])
        self.assertFalse(result['collection_allowed'])
        with self.assertRaisesRegex(ValueError, 'COLLECTION_LAYOUT_CARD_MISSING'):
            bind_card(after, rule(), 'c0')

    def test_scroll_same_image_or_same_frame_is_rejected(self):
        before = packet()
        for section in ('frame_id', 'frame_sha256'):
            after = scrolled(before)
            for key in ('collection_observation', 'collection_layout'):
                after[key][section] = before[key][section]
            if section == 'frame_sha256':
                after['frames'][-1]['sha256'] = before['frames'][-1]['sha256']
            with self.subTest(section=section), self.assertRaisesRegex(ValueError, 'COLLECTION_REOBSERVATION_REQUIRED'):
                rebind_after_scroll(after, rule(), self.token(before))

    def test_wrong_scroll_direction_rejected(self):
        before = packet()
        before['collection_layout']['scrollbar']['thumb_bounds'][1] = 400
        after = scrolled(before)
        after['collection_layout']['scrollbar']['thumb_bounds'][1] = 360
        with self.assertRaisesRegex(ValueError, 'COLLECTION_SCROLL_DIRECTION_CONFLICT'):
            rebind_after_scroll(after, rule(), self.token(before))

    def test_reflow_or_scrollbar_resize_rejected(self):
        before = packet()
        for key in ('track', 'thumb', 'viewport'):
            after = scrolled(before)
            if key == 'track':
                after['collection_layout']['scrollbar']['track_bounds'][3] -= 10
            elif key == 'thumb':
                after['collection_layout']['scrollbar']['thumb_bounds'][3] += 10
            else:
                after['collection_layout']['listing_viewport'][3] -= 10
            with self.subTest(key=key), self.assertRaises(ValueError):
                rebind_after_scroll(after, rule(), self.token(before))

    def test_bottom_is_not_completion(self):
        before = packet()
        before['collection_layout']['scrollbar']['thumb_bounds'][1] = 1050
        with self.assertRaisesRegex(ValueError, 'COLLECTION_SCROLL_PROGRESS_UNPROVEN'):
            rebind_after_scroll(next_frame(before), rule(), self.token(before))

    def test_up_scroll_can_be_proven_without_reusing_coordinates(self):
        before = packet()
        before['collection_layout']['scrollbar']['thumb_bounds'][1] = 400
        token = scroll_plan(before, rule(), [], delta=120)['steps'][0]['lease']
        after = scrolled(before)
        after['collection_layout']['scrollbar']['thumb_bounds'][1] = 350
        self.assertEqual(rebind_after_scroll(after, rule(), token)['scrollbar_shift_pixels'], -50)


class NoRepeatTests(unittest.TestCase):
    white = dict(selected=True, favorite_warm_fraction=0, favorite_bright_fraction=.1)
    gold = dict(selected=True, favorite_warm_fraction=.12, favorite_bright_fraction=0)

    def test_confirmed_identity_preserved_after_price_change(self):
        item = candidate()
        item['price'] = '199'
        result = collection_disposition(item, self.gold, [record()])
        self.assertEqual(result['disposition'], 'preserve_confirmed')
        self.assertFalse(result['allow_toggle'])

    def test_existing_unknown_gold_is_preserved(self):
        self.assertEqual(collection_disposition(candidate(), self.gold, [])['disposition'], 'preserve_existing')

    def test_trailing_wear_zero_is_not_a_new_identity(self):
        item = dict(candidate(), wear='2.1986420')
        result = collection_disposition(item, self.gold, [record()])
        self.assertEqual(result['disposition'], 'preserve_confirmed')
        self.assertEqual(result['key'], record()['key'])
        with self.assertRaisesRegex(ValueError, 'COLLECTION_CONFIRMED_STATE_CONFLICT'):
            collection_disposition(item, self.white, [record()])

    def test_non_observed_numeric_wear_is_not_an_identity(self):
        for wear in ('NaN', 'Infinity', '2e0', '-0.2', '2198642', '2.1986420000'):
            with self.subTest(wear=wear), self.assertRaisesRegex(ValueError, 'COLLECTION_CANDIDATE_IDENTITY'):
                collection_disposition(dict(candidate(), wear=wear), self.white, [])

    def test_normalized_duplicate_journal_identities_stop(self):
        with self.assertRaisesRegex(ValueError, 'COLLECTION_JOURNAL_IDENTITY'):
            collection_disposition(candidate(), self.gold,
                                   [record(), record(item=dict(candidate(), wear='2.1986420'))])

    def test_confirmed_identity_shown_white_requires_reconciliation(self):
        with self.assertRaisesRegex(ValueError, 'COLLECTION_CONFIRMED_STATE_CONFLICT'):
            collection_disposition(candidate(), self.white, [record()])

    def test_pending_attempt_blocks_all_scroll_and_dispositions(self):
        for status in ('prepared', 'dispatched', 'input_uncertain'):
            with self.subTest(status=status), self.assertRaisesRegex(ValueError, 'COLLECTION_PENDING_RECONCILIATION'):
                scroll_plan(packet(), rule(), [record(status)])
            with self.subTest(status=status), self.assertRaisesRegex(ValueError, 'COLLECTION_PENDING_RECONCILIATION'):
                collection_disposition(candidate(), self.gold, [record(status)])

    def test_unknown_color_or_selection_does_not_become_white(self):
        for selected in ({}, dict(self.white, selected=False),
                         dict(self.white, favorite_warm_fraction=float('nan')),
                         dict(self.white, favorite_bright_fraction=True),
                         dict(self.white, favorite_warm_fraction=.02)):
            with self.subTest(selected=selected), self.assertRaises(ValueError):
                collection_disposition(candidate(), selected, [])

    def test_unseen_white_still_needs_candidate_and_durable_prepare(self):
        result = collection_disposition(candidate(), self.white, [])
        self.assertEqual(result['disposition'], 'unseen_white_requires_candidate_and_journal')
        self.assertFalse(result['allow_toggle'])

    def test_corrupt_journal_identity_or_status_rejected(self):
        for bad in (dict(record(), key='f' * 64), dict(record(), status='failed'),
                    dict(record(), candidate={}), dict(record(), candidate=None)):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                collection_disposition(candidate(), self.gold, [bad])
        with self.assertRaisesRegex(ValueError, 'COLLECTION_JOURNAL_IDENTITY'):
            collection_disposition(candidate(), self.gold, [record(), record()])


class HistoricalEvidenceBoundaryTests(unittest.TestCase):
    def test_historical_packet_does_not_claim_dynamic_layout(self):
        path = ROOT / 'artifacts/m2_savedvalue_collection/live_series_s6_v5/live_5.json'
        if not path.exists():
            self.skipTest('Optional local historical field packet is absent; synthetic contract tests still run.')
        value = json.loads(path.read_text(encoding='utf-8'))
        captured = [step['result'] for step in value['steps'] if 'result' in step][-1]
        self.assertEqual(captured['collection_selected_card']['index'], 5)
        with self.assertRaisesRegex(ValueError, 'COLLECTION_LAYOUT_MISSING'):
            observe_layout(captured)


if __name__ == '__main__':
    unittest.main(verbosity=2)
