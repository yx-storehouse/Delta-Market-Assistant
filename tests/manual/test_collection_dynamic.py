"""Offline dynamic-layout consumer and session wiring; no live calibration claims."""
import copy
import json
from pathlib import Path
import tempfile
import unittest

import test_collection_live_session as fixtures
from collection_candidate import match_selected_first_card
from collection_live_session import ForegroundSession
from collection_scroll import bind_card, selection_plan, scroll_plan, same_card_geometry
from cursor_motion import MotionInterrupted


def rule():
    return fixtures.snapshot()['rows'][0]


def dynamic_packet(digest='a', *, x=997, top=441, card_id='observed-row', selected=True, gold=False, thumb_y=350):
    result = fixtures.packet(digest, gold=gold)
    projection = result['collection_observation']
    fields = [copy.deepcopy(w) for w in next(r for r in projection['regions'] if r['kind'] == 'first_card_fields')['words']]
    for word in fields:
        word['x'] += x - 120
        word['y'] += top - 303
    card = dict(id=card_id, bounds=[x, top, 865, 266], fields_bounds=[x, top + 222, 865, 44],
                condition_bounds=[x, top + 222, 100, 44], price_bounds=[x + 730, top + 222, 135, 44],
                edges=dict(top=True, bottom=True, left=True, right=True), selected=selected)
    projection['regions'].append(dict(kind='card_fields', card_id=card_id, bounds=card['fields_bounds'],
                                       ok=True, truncated=False, words=fields))
    # Keep a contradictory legacy strip: the new parser must never fall back
    # to the old six-card region when a dynamic layout is present.
    next(r for r in projection['regions'] if r['kind'] == 'first_card_fields')['words'][-1]['text'] = '9999'
    result['collection_layout'] = dict(schema='collection-layout-v1', detector='visible_edges', complete=True,
        same_frame=True, frame_id=projection['frame_id'], frame_sha256=projection['frame_sha256'],
        viewport=[2560, 1440], listing_viewport=[120, 303, 1742, 947], cards=[card],
        scrollbar=dict(track_bounds=[1870, 303, 8, 947], thumb_bounds=[1870, thumb_y, 8, 200]))
    result['collection_selected_card'].pop('index')
    result['collection_selected_card'].update(card_id=card_id, selected=selected, same_frame=True,
        frame_id=projection['frame_id'], frame_sha256=projection['frame_sha256'],
        bounds=card['bounds'], fields_bounds=card['fields_bounds'])
    return result


def transient_packet(digest='b'):
    result = dynamic_packet(digest)
    result['collection_layout'].update(complete=False, cards=[], error='E_COLLECTION_CARD_VERTICAL_EDGE')
    return result


def missing_price_packet(digest='b', *, gold=True):
    result = dynamic_packet(digest, gold=gold)
    result['collection_observation']['regions'][-1]['words'].pop()
    return result


class DynamicCandidateTests(unittest.TestCase):
    def test_gray_white_outline_tolerance_is_small_and_bounded(self):
        original = dict(bounds=[997, 441, 865, 266], fields_bounds=[997, 663, 865, 44])
        for pixels in (1, 2, 3):
            expanded = dict(bounds=[997-pixels, 441-pixels, 865+2*pixels, 266+2*pixels],
                            fields_bounds=[997-pixels, 663-pixels, 865, 44])
            self.assertTrue(same_card_geometry(original, expanded))
        self.assertFalse(same_card_geometry(original, dict(bounds=[993, 437, 873, 274],
                                                          fields_bounds=[993, 659, 865, 44])))
        self.assertFalse(same_card_geometry(original, original, tolerance=4))

    def test_real_run03_gray_white_horizontal_uncertainty_preserves_measured_geometry(self):
        gray = dict(bounds=[120, 306, 871, 260], fields_bounds=[121, 523, 869, 42])
        white = dict(bounds=[119, 304, 866, 264], fields_bounds=[120, 525, 864, 42])
        original = copy.deepcopy((gray, white))
        self.assertTrue(same_card_geometry(gray, white))
        self.assertFalse(same_card_geometry(gray, white, horizontal_tolerance=3))
        self.assertEqual((gray, white), original)

    def test_axial_tolerance_rejects_over_eight_x_over_three_y_and_other_card(self):
        first = dict(bounds=[120, 304, 866, 264], fields_bounds=[121, 525, 864, 42])
        for dx, dy, expected in ((8, 0, True), (-8, 0, True), (9, 0, False), (0, 4, False),
                                  (877, 0, False), (0, 275, False)):
            other = copy.deepcopy(first)
            for bounds in (other['bounds'], other['fields_bounds']):
                bounds[0] += dx
                bounds[1] += dy
            with self.subTest(dx=dx, dy=dy):
                self.assertEqual(same_card_geometry(first, other), expected)
        self.assertFalse(same_card_geometry(first, first, horizontal_tolerance=9))

    def test_axial_tolerance_does_not_replace_98_percent_overlap(self):
        small = dict(bounds=[120, 304, 100, 100], fields_bounds=[121, 380, 98, 20])
        moved = dict(bounds=[128, 304, 100, 100], fields_bounds=[129, 380, 98, 20])
        self.assertFalse(same_card_geometry(small, moved))

    def test_observed_fields_ignore_legacy_grid_and_index(self):
        packet = dynamic_packet(x=183, top=467)
        packet['collection_selected_card']['index'] = 999
        candidate = match_selected_first_card(packet, rule())
        self.assertEqual(candidate['price'], '230')
        self.assertEqual(candidate['wear'], '0.187079')
        self.assertEqual(candidate['geometry_mode'], 'observed_dynamic')
        self.assertEqual(candidate['card_bounds'], [183, 467, 865, 266])

    def test_invalid_dynamic_layout_does_not_fall_back_to_legacy(self):
        packet = dynamic_packet()
        packet['collection_layout']['complete'] = False
        with self.assertRaisesRegex(ValueError, 'COLLECTION_LAYOUT_UNPROVEN'):
            match_selected_first_card(packet, rule())

    def test_selected_card_frame_and_bounds_must_agree(self):
        for key, value in [('frame_id', 'old'), ('frame_sha256', 'f' * 64), ('same_frame', False),
                           ('card_id', 'another-row'), ('bounds', [120, 303, 865, 266])]:
            packet = dynamic_packet()
            packet['collection_selected_card'][key] = value
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, 'COLLECTION_DYNAMIC_SELECTED_BINDING'):
                match_selected_first_card(packet, rule())

    def test_fields_must_belong_to_current_selected_card(self):
        for key, value in [('card_id', 'other-row'), ('bounds', [120, 525, 865, 44]), ('truncated', True)]:
            packet = dynamic_packet()
            region = next(r for r in packet['collection_observation']['regions'] if r['kind'] == 'card_fields')
            region[key] = value
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, 'COLLECTION_DYNAMIC_FIELD_BINDING'):
                match_selected_first_card(packet, rule())

    def test_duplicate_row_fields_rejected(self):
        packet = dynamic_packet()
        packet['collection_observation']['regions'].append(copy.deepcopy(packet['collection_observation']['regions'][-1]))
        with self.assertRaisesRegex(ValueError, 'COLLECTION_DYNAMIC_FIELD_BINDING'):
            match_selected_first_card(packet, rule())

    def test_cross_row_word_rejected(self):
        packet = dynamic_packet()
        region = packet['collection_observation']['regions'][-1]
        region['words'][-1]['y'] += 275
        with self.assertRaisesRegex(ValueError, 'COLLECTION_DYNAMIC_FIELD_WORDS'):
            match_selected_first_card(packet, rule())

    def test_detached_price_digit_outside_subroi_is_not_dropped(self):
        packet = dynamic_packet()
        packet['collection_observation']['regions'][-1]['words'].append(
            dict(text='8', x=1300, y=676, width=11, height=19))
        with self.assertRaisesRegex(ValueError, 'COLLECTION_DYNAMIC_UNASSOCIATED_FIELD'):
            match_selected_first_card(packet, rule())

    def test_dynamic_english_price_boxes_use_observed_strip_height(self):
        for height in (23.410123671741207, 24.734591034983765, 29.4):
            packet = dynamic_packet()
            price = packet['collection_observation']['regions'][-1]['words'][-1]
            price['height'] = height
            price['y'] = packet['collection_layout']['cards'][0]['price_bounds'][1] + 8
            with self.subTest(height=height):
                self.assertEqual(match_selected_first_card(packet, rule())['price'], '230')

    def test_dynamic_oversize_price_is_not_silently_discarded(self):
        packet = dynamic_packet()
        price = packet['collection_observation']['regions'][-1]['words'][-1]
        bounds = packet['collection_layout']['cards'][0]['price_bounds']
        price.update(height=bounds[3] * 0.7 + 0.01, y=bounds[1] + 6)
        with self.assertRaisesRegex(ValueError, 'COLLECTION_DYNAMIC_UNASSOCIATED_FIELD'):
            match_selected_first_card(packet, rule())

    def test_dynamic_taller_price_keeps_token_and_alignment_checks(self):
        for mutation, expected in ((dict(text='23O'), 'COLLECTION_PRICE_TOKEN'),
                                   (dict(x=1750), 'COLLECTION_PRICE_ALIGNMENT')):
            packet = dynamic_packet()
            price = packet['collection_observation']['regions'][-1]['words'][-1]
            price.update(height=24.734591034983765, y=671, **mutation)
            with self.subTest(mutation=mutation), self.assertRaisesRegex(ValueError, expected):
                match_selected_first_card(packet, rule())

    def test_subroi_cannot_cross_fields_or_overlap(self):
        for bounds in ([10, 20, 135, 44], [997, 663, 135, 44]):
            packet = dynamic_packet()
            packet['collection_layout']['cards'][0]['price_bounds'] = bounds
            with self.subTest(bounds=bounds), self.assertRaisesRegex(ValueError, 'COLLECTION_LAYOUT_FIELD_ROI'):
                match_selected_first_card(packet, rule())

    def test_missing_subroi_not_inferred_from_old_grid(self):
        packet = dynamic_packet()
        del packet['collection_layout']['cards'][0]['price_bounds']
        with self.assertRaisesRegex(ValueError, 'COLLECTION_DYNAMIC_FIELD_ROI'):
            match_selected_first_card(packet, rule())

    def test_half_card_has_no_candidate(self):
        packet = dynamic_packet()
        packet['collection_layout']['cards'][0]['edges']['bottom'] = False
        with self.assertRaisesRegex(ValueError, 'COLLECTION_DYNAMIC_SELECTION_UNPROVEN'):
            match_selected_first_card(packet, rule())


class DynamicSessionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.snapshot_path = self.root / 'artifacts/trial/input/snapshot.json'
        self.snapshot_path.parent.mkdir(parents=True)
        self.snapshot_path.write_text(json.dumps(fixtures.snapshot()), encoding='utf-8')
        self.clock = fixtures.Clock()
        self.backend = fixtures.FakeBackend(self.clock)

    def session(self):
        return ForegroundSession('artifacts/trial/run.json', root=self.root, backend=self.backend,
                                 now=self.clock.now, wait=self.clock.wait)

    def test_selection_reobserve_collection_and_dynamic_receipt(self):
        before = dynamic_packet(selected=False)
        selected = dynamic_packet('b', card_id='next-frame-row')
        after = dynamic_packet('c', card_id='receipt-frame-row', gold=True)
        self.backend.replies = [before, selected, after]
        session = self.session()
        with session:
            session.perform(fixtures.capture(collection_layout=True))
            plan = selection_plan(session.previous, rule(), 'observed-row')
            session.perform(plan['steps'][0])
            self.assertIsNotNone(session.pending_geometry)
            session.perform(plan['steps'][1])
            self.assertIsNone(session.pending_geometry)
            self.assertEqual(session.selected_lease['card']['id'], 'next-frame-row')
            action = session.perform(fixtures.collect())
            self.assertEqual(action['collection_attempt']['card_id'], 'next-frame-row')
            session.perform(fixtures.capture(expect_collection_added=True, receipt_if_pending=True))
            self.assertIsNone(session.pending)
        self.assertTrue(session.report['passed'])
        self.assertEqual(session.report['manual_clicks'], 2)
        self.assertTrue(all('--collection-layout' in event[1] for event in self.backend.events
                            if isinstance(event, tuple) and event[0] == 'capture'))

    def test_unselected_layout_needs_no_fields_until_selected_fresh_frame(self):
        before = dynamic_packet(selected=False)
        before['collection_observation']['regions'] = [r for r in before['collection_observation']['regions']
                                                       if r['kind'] not in ('card_fields', 'first_card_fields')]
        selected = dynamic_packet('b')
        selected['collection_observation']['regions'] = [r for r in selected['collection_observation']['regions']
                                                        if r['kind'] != 'first_card_fields']
        self.backend.replies = [before, selected]
        session = self.session()
        with session:
            session.perform(fixtures.capture(collection_layout=True))
            plan = selection_plan(session.previous, rule(), 'observed-row')
            for step in plan['steps']:
                session.perform(step)
            candidate = match_selected_first_card(session.previous, rule())
            self.assertEqual(candidate['source_frame_id'], 'fake:b')
            self.assertEqual(candidate['price'], '230')
        self.assertTrue(session.report['passed'])
        self.assertEqual(session.report['manual_clicks'], 1)

    def test_selection_cannot_advance_without_new_geometry(self):
        self.backend.replies = [dynamic_packet(selected=False)]
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'COLLECTION_GEOMETRY_REOBSERVATION_REQUIRED'):
            with session:
                session.perform(fixtures.capture(collection_layout=True))
                session.perform(selection_plan(session.previous, rule(), 'observed-row')['steps'][0])
                session.perform(fixtures.collect())
        self.assertEqual(session.report['manual_clicks'], 1)

    def test_selection_geometry_drift_stops_and_keeps_evidence(self):
        self.backend.replies = [dynamic_packet(selected=False), dynamic_packet('b', top=400)]
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'BATCH_STEP_FAILED'):
            with session:
                session.perform(fixtures.capture(collection_layout=True))
                session.perform(selection_plan(session.previous, rule(), 'observed-row')['steps'][0])
                session.perform(fixtures.capture())
        self.assertIn('COLLECTION_SELECTION_GEOMETRY_CHANGED', session.steps[-1]['result']['collection_geometry_error'])
        self.assertIsNotNone(session.pending_geometry)
        self.assertIsNone(session.pending)
        self.assertEqual(session.steps[-1]['attempt_count'], 1)

    def test_transient_layout_reobserves_three_new_frames_without_repeating_click(self):
        self.backend.replies = [dynamic_packet(selected=False), transient_packet('b'),
                                transient_packet('c'), dynamic_packet('d')]
        session = self.session()
        pending_seen = []
        original_capture = self.backend.capture
        def observed_capture(command, timeout):
            if session.pending_geometry is not None:
                pending_seen.append(copy.deepcopy(session.pending_geometry))
            return original_capture(command, timeout)
        self.backend.capture = observed_capture
        with session:
            session.perform(fixtures.capture(collection_layout=True))
            for step in selection_plan(session.previous, rule(), 'observed-row')['steps']:
                session.perform(step)
            self.assertIsNone(session.pending_geometry)
            self.assertEqual(session.selected_lease['frame_id'], 'fake:d')
        self.assertEqual(session.steps[-1]['attempt_count'], 3)
        self.assertEqual(len(pending_seen), 3)
        self.assertTrue(all(p == pending_seen[0] for p in pending_seen))
        self.assertEqual(session.report['manual_clicks'], 1)
        self.assertEqual([a['collection_layout_retryable'] for a in session.steps[-1]['attempts']],
                         [True, True, False])

    def test_transient_layout_stops_at_three_and_preserves_pending_geometry(self):
        self.backend.replies = [dynamic_packet(selected=False), transient_packet('b'),
                                transient_packet('c'), transient_packet('d'), dynamic_packet('e')]
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'BATCH_STEP_FAILED'):
            with session:
                session.perform(fixtures.capture(collection_layout=True))
                for step in selection_plan(session.previous, rule(), 'observed-row')['steps']:
                    session.perform(step)
        self.assertEqual(session.steps[-1]['attempt_count'], 3)
        self.assertEqual(len(self.backend.replies), 1)
        self.assertEqual(session.report['manual_clicks'], 1)
        self.assertIsNotNone(session.pending_geometry)
        self.assertTrue(session.report['ide_restored'])

    def test_transient_retry_requires_independent_frame_id(self):
        self.backend.replies = [dynamic_packet(selected=False), transient_packet('b'), dynamic_packet('b')]
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'BATCH_STEP_FAILED'):
            with session:
                session.perform(fixtures.capture(collection_layout=True))
                for step in selection_plan(session.previous, rule(), 'observed-row')['steps']:
                    session.perform(step)
        self.assertEqual(session.steps[-1]['attempt_count'], 2)
        self.assertEqual(session.steps[-1]['result']['collection_geometry_error'],
                         'COLLECTION_REOBSERVATION_REQUIRED')
        self.assertIsNotNone(session.pending_geometry)
        self.assertEqual(session.report['manual_clicks'], 1)

    def test_transient_retry_never_reclaims_foreground(self):
        self.backend.replies = [dynamic_packet(selected=False), transient_packet('b')]
        session = self.session()
        original_capture = self.backend.capture
        def lose_focus(command, timeout):
            response = original_capture(command, timeout)
            if session.pending_geometry is not None:
                self.backend.active = False
            return response
        self.backend.capture = lose_focus
        with self.assertRaisesRegex(RuntimeError, 'BATCH_FOREGROUND_LOST'):
            with session:
                session.perform(fixtures.capture(collection_layout=True))
                for step in selection_plan(session.previous, rule(), 'observed-row')['steps']:
                    session.perform(step)
        self.assertEqual(len(session.steps[-1]['capture_attempts']), 1)
        self.assertEqual(session.report['enter_calls'], 1)
        self.assertEqual(session.report['manual_clicks'], 1)

    def test_transient_layout_with_invalid_viewport_is_not_retried(self):
        invalid = transient_packet()
        invalid['collection_layout']['listing_viewport'][0] = -1
        self.backend.replies = [invalid, dynamic_packet('c')]
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'BATCH_STEP_FAILED'):
            with session:
                session.perform(fixtures.capture(collection_layout=True))
        self.assertEqual(session.steps[-1]['attempt_count'], 1)
        self.assertEqual(len(self.backend.replies), 1)
        self.assertEqual(session.report['manual_clicks'], 0)

    def test_transient_layout_with_unbound_frame_is_not_retried(self):
        invalid = transient_packet()
        invalid['collection_layout']['frame_id'] = 'another-frame'
        self.backend.replies = [invalid, dynamic_packet('c')]
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'BATCH_STEP_FAILED'):
            with session:
                session.perform(fixtures.capture(collection_layout=True))
        self.assertEqual(session.steps[-1]['attempt_count'], 1)

    def test_unknown_native_layout_error_is_not_retried(self):
        invalid = transient_packet()
        invalid['collection_layout']['error'] = 'E_COLLECTION_CARD_HEIGHT'
        self.backend.replies = [invalid, dynamic_packet('c')]
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'BATCH_STEP_FAILED'):
            with session:
                session.perform(fixtures.capture(collection_layout=True))
        self.assertEqual(session.steps[-1]['attempt_count'], 1)

    def test_gray_white_border_change_rebinds_fresh_fields_without_old_coordinates(self):
        before = dynamic_packet(selected=False)
        selected = dynamic_packet('b', x=995, top=439, card_id='white-contour')
        before['collection_layout']['listing_viewport'][2] += 4
        selected['collection_layout']['listing_viewport'][2] += 4
        observed_bounds = [995, 439, 869, 270]
        selected['collection_layout']['cards'][0]['bounds'] = observed_bounds
        selected['collection_selected_card']['bounds'] = observed_bounds
        self.backend.replies = [before, selected]
        session = self.session()
        with session:
            session.perform(fixtures.capture(collection_layout=True))
            for step in selection_plan(session.previous, rule(), 'observed-row')['steps']:
                session.perform(step)
            candidate = match_selected_first_card(session.previous, rule())
            self.assertEqual(candidate['card_bounds'], observed_bounds)
            self.assertEqual(candidate['fields_bounds'], [995, 661, 865, 44])
            self.assertEqual(candidate['price'], '230')
        self.assertTrue(session.report['passed'])

    def test_scroll_reobserves_current_layout_and_invalidates_old_targets(self):
        self.backend.replies = [dynamic_packet(), dynamic_packet('b', top=386, thumb_y=390)]
        session = self.session()
        with session:
            session.perform(fixtures.capture(collection_layout=True))
            lease = bind_card(session.previous, rule(), 'observed-row')
            session.perform(scroll_plan(session.previous, rule(), [])['steps'][0])
            session.perform(fixtures.capture())
            self.assertIsNone(session.pending_geometry)
            result = session.previous['collection_geometry_rebind']
            self.assertEqual(result['kind'], 'scroll')
            self.assertEqual(result['evidence']['scrollbar_shift_pixels'], 40)
            self.assertFalse(result['evidence']['page_exhausted'])
            self.assertNotEqual(lease['frame_sha256'], session.previous['collection_layout']['frame_sha256'])
        self.assertEqual(session.report['manual_clicks'], 0)

    def test_hash_change_without_scrollbar_movement_is_not_progress(self):
        self.backend.replies = [dynamic_packet(), dynamic_packet('b')]
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'BATCH_STEP_FAILED'):
            with session:
                session.perform(fixtures.capture(collection_layout=True))
                session.perform(scroll_plan(session.previous, rule(), [])['steps'][0])
                session.perform(fixtures.capture())
        self.assertEqual(session.steps[-1]['result']['collection_geometry_error'], 'COLLECTION_SCROLL_PROGRESS_UNPROVEN')

    def test_scroll_selected_partial_can_reselect_current_complete_card(self):
        before = dynamic_packet()
        scrolled = dynamic_packet('b', selected=False, thumb_y=390)
        partial = dict(id='partial-old-selection', bounds=[997, 303, 865, 90], fields_bounds=None,
                       edges=dict(top=False, bottom=True, left=True, right=True), selected=True)
        scrolled['collection_layout']['cards'].insert(0, partial)
        scrolled['collection_selected_card'].update(card_id=partial['id'], bounds=partial['bounds'],
            fields_bounds=None, selected=True, geometry_lost=True)
        selected = dynamic_packet('c', thumb_y=390)
        self.backend.replies = [before, scrolled, selected]
        session = self.session()
        with session:
            session.perform(fixtures.capture(collection_layout=True))
            for step in scroll_plan(session.previous, rule(), [])['steps']:
                session.perform(step)
            self.assertIsNone(session.pending_geometry)
            with self.assertRaisesRegex(ValueError, 'COLLECTION_DYNAMIC_SELECTION_UNPROVEN'):
                match_selected_first_card(session.previous, rule())
            for step in selection_plan(session.previous, rule(), 'observed-row')['steps']:
                session.perform(step)
            self.assertIsNone(session.pending_geometry)
            self.assertEqual(match_selected_first_card(session.previous, rule())['price'], '230')
        self.assertTrue(session.report['passed'])
        self.assertEqual(session.report['manual_clicks'], 1)

    def test_dynamic_receipt_must_preserve_selected_geometry(self):
        self.backend.replies = [dynamic_packet(), dynamic_packet('b', top=416, gold=True)]
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'BATCH_STEP_FAILED'):
            with session:
                session.perform(fixtures.capture(collection_layout=True))
                session.perform(fixtures.collect())
                session.perform(fixtures.capture(expect_collection_added=True, receipt_if_pending=True))
        self.assertIsNotNone(session.pending)
        self.assertFalse(session.steps[-1]['result']['collection_receipt_passed'])
        self.assertEqual(session.report['manual_clicks'], 1)
        self.assertEqual(session.steps[-1]['attempt_count'], 1)

    def test_receipt_tail_edge_retry_keeps_one_star_and_confirms_only_fresh_gold(self):
        missed = transient_packet('b')
        missed['collection_layout']['error'] = 'E_COLLECTION_PARTIAL_SPANS_ROWS'
        missed['collection_selected_card']['selected'] = False
        self.backend.replies = [dynamic_packet(), missed, transient_packet('c'), dynamic_packet('d', gold=True)]
        session = self.session()
        pending_seen, statuses = [], []
        original_capture = self.backend.capture
        def observed_capture(command, timeout):
            if session.pending is not None:
                pending_seen.append(copy.deepcopy(session.pending))
                statuses.append(json.loads(next(session.journal.directory.glob('*.json')).read_text('utf-8'))['status'])
            return original_capture(command, timeout)
        self.backend.capture = observed_capture
        with session:
            session.perform(fixtures.capture(collection_layout=True))
            session.perform(fixtures.collect())
            session.perform(fixtures.capture(expect_collection_added=True, receipt_if_pending=True))
            self.assertIsNone(session.pending)
        self.assertEqual(session.steps[-1]['attempt_count'], 3)
        self.assertEqual(session.report['manual_clicks'], 1)
        self.assertEqual(statuses, ['dispatched'] * 3)
        self.assertTrue(all(p == pending_seen[0] for p in pending_seen))
        document = json.loads(next(session.journal.directory.glob('*.json')).read_text('utf-8'))
        self.assertEqual(document['status'], 'confirmed')
        self.assertEqual(document['receipt']['source_frame_id'], 'fake:d')

    def test_receipt_edge_retries_exhausted_keep_pending_without_new_input(self):
        self.backend.replies = [dynamic_packet(), transient_packet('b'), transient_packet('c'),
                                transient_packet('d'), dynamic_packet('e', gold=True)]
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'BATCH_STEP_FAILED'):
            with session:
                session.perform(fixtures.capture(collection_layout=True))
                session.perform(fixtures.collect())
                session.perform(fixtures.capture(expect_collection_added=True, receipt_if_pending=True))
        self.assertEqual(session.steps[-1]['attempt_count'], 3)
        self.assertIsNotNone(session.pending)
        self.assertEqual(session.report['manual_clicks'], 1)
        self.assertEqual(len(self.backend.replies), 1)
        document = json.loads(next(session.journal.directory.glob('*.json')).read_text('utf-8'))
        self.assertEqual(document['status'], 'dispatched')

    def test_receipt_identity_conflict_after_edge_retry_stops_immediately(self):
        conflict = dynamic_packet('c', gold=True)
        region = next(r for r in conflict['collection_observation']['regions'] if r['kind'] == 'selected_detail')
        region['words'][0]['text'] = 'S(0.187080)'
        self.backend.replies = [dynamic_packet(), transient_packet('b'), conflict, dynamic_packet('d', gold=True)]
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'BATCH_STEP_FAILED'):
            with session:
                session.perform(fixtures.capture(collection_layout=True))
                session.perform(fixtures.collect())
                session.perform(fixtures.capture(expect_collection_added=True, receipt_if_pending=True))
        self.assertEqual(session.steps[-1]['attempt_count'], 2)
        self.assertFalse(session.steps[-1]['result']['collection_receipt_passed'])
        self.assertEqual(len(self.backend.replies), 1)
        self.assertIsNotNone(session.pending)
        self.assertEqual(session.report['manual_clicks'], 1)

    def test_incomplete_layout_wrong_product_receipt_is_not_retried(self):
        wrong = transient_packet('b')
        title = next(r for r in wrong['collection_observation']['regions'] if r['kind'] == 'product_title')
        title['words'][0]['text'] = 'P90冲锋枪-天命'
        self.backend.replies = [dynamic_packet(), wrong, dynamic_packet('c', gold=True)]
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'BATCH_STEP_FAILED'):
            with session:
                session.perform(fixtures.capture(collection_layout=True))
                session.perform(fixtures.collect())
                session.perform(fixtures.capture(expect_collection_added=True, receipt_if_pending=True))
        self.assertEqual(session.steps[-1]['attempt_count'], 1)
        self.assertIsNotNone(session.pending)

    def test_failed_capture_never_confirms_otherwise_matching_gold_receipt(self):
        failed = dynamic_packet('b', gold=True)
        failed['ocr_passed'] = False
        self.backend.replies = [dynamic_packet(), failed]
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'BATCH_STEP_FAILED'):
            with session:
                session.perform(fixtures.capture(collection_layout=True))
                session.perform(fixtures.collect())
                session.perform(fixtures.capture(expect_collection_added=True, receipt_if_pending=True))
        self.assertEqual(session.steps[-1]['attempt_count'], 1)
        self.assertIsNotNone(session.pending)
        document = json.loads(next(session.journal.directory.glob('*.json')).read_text('utf-8'))
        self.assertEqual(document['status'], 'dispatched')

    def test_missing_price_receipt_reobserves_instead_of_borrowing_old_price(self):
        self.backend.replies = [dynamic_packet(), missing_price_packet('b'), dynamic_packet('c', gold=True)]
        session = self.session()
        with session:
            session.perform(fixtures.capture(collection_layout=True))
            session.perform(fixtures.collect())
            session.perform(fixtures.capture(expect_collection_added=True, receipt_if_pending=True))
            self.assertIsNone(session.pending)
        self.assertEqual(session.steps[-1]['attempt_count'], 2)
        failed = session.steps[-1]['attempts'][0]
        self.assertTrue(failed['collection_receipt_retryable'])
        self.assertEqual(failed['result']['collection_receipt_error'], 'COLLECTION_PRICE_MISSING')
        self.assertFalse(failed['result']['collection_receipt_passed'])
        self.assertEqual(session.report['manual_clicks'], 1)
        document = json.loads(next(session.journal.directory.glob('*.json')).read_text('utf-8'))
        self.assertEqual(document['receipt']['source_frame_id'], 'fake:c')

    def test_missing_price_does_not_hide_explicit_wear_identity_conflict(self):
        conflict = missing_price_packet('b')
        region = next(r for r in conflict['collection_observation']['regions'] if r['kind'] == 'selected_detail')
        region['words'][0]['text'] = 'S(0.187080)'
        self.backend.replies = [dynamic_packet(), conflict, dynamic_packet('c', gold=True)]
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'BATCH_STEP_FAILED'):
            with session:
                session.perform(fixtures.capture(collection_layout=True))
                session.perform(fixtures.collect())
                session.perform(fixtures.capture(expect_collection_added=True, receipt_if_pending=True))
        self.assertEqual(session.steps[-1]['attempt_count'], 1)
        self.assertEqual(session.steps[-1]['result']['collection_receipt_error'],
                         'COLLECTION_RECEIPT_IDENTITY_CONFLICT')
        self.assertIsNotNone(session.pending)
        self.assertEqual(session.report['manual_clicks'], 1)

    def test_missing_price_does_not_hide_explicit_geometry_conflict(self):
        conflict = missing_price_packet('b')
        card = conflict['collection_layout']['cards'][0]
        card['bounds'][0] -= 9
        card['bounds'][2] += 9
        conflict['collection_selected_card']['bounds'] = card['bounds']
        conflict['collection_layout']['listing_viewport'][0] -= 9
        conflict['collection_layout']['listing_viewport'][2] += 9
        self.backend.replies = [dynamic_packet(), conflict, dynamic_packet('c', gold=True)]
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'BATCH_STEP_FAILED'):
            with session:
                session.perform(fixtures.capture(collection_layout=True))
                session.perform(fixtures.collect())
                session.perform(fixtures.capture(expect_collection_added=True, receipt_if_pending=True))
        self.assertEqual(session.steps[-1]['attempt_count'], 1)
        self.assertEqual(session.steps[-1]['result']['collection_receipt_error'],
                         'COLLECTION_RECEIPT_GEOMETRY_CONFLICT')
        self.assertIsNotNone(session.pending)

    def test_present_but_misaligned_price_is_not_retried(self):
        conflict = dynamic_packet('b', gold=True)
        conflict['collection_observation']['regions'][-1]['words'][-1]['x'] = 1750
        self.backend.replies = [dynamic_packet(), conflict, dynamic_packet('c', gold=True)]
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'BATCH_STEP_FAILED'):
            with session:
                session.perform(fixtures.capture(collection_layout=True))
                session.perform(fixtures.collect())
                session.perform(fixtures.capture(expect_collection_added=True, receipt_if_pending=True))
        self.assertEqual(session.steps[-1]['attempt_count'], 1)
        self.assertEqual(session.steps[-1]['result']['collection_receipt_error'], 'COLLECTION_PRICE_ALIGNMENT')

    def test_missing_price_retry_shared_budget_and_changed_price_not_accepted(self):
        changed = dynamic_packet('d', gold=True)
        changed['collection_observation']['regions'][-1]['words'][-1]['text'] = '231'
        self.backend.replies = [dynamic_packet(), transient_packet('b'), missing_price_packet('c'), changed]
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'BATCH_STEP_FAILED'):
            with session:
                session.perform(fixtures.capture(collection_layout=True))
                session.perform(fixtures.collect())
                session.perform(fixtures.capture(expect_collection_added=True, receipt_if_pending=True))
        self.assertEqual(session.steps[-1]['attempt_count'], 3)
        self.assertFalse(session.steps[-1]['result']['collection_receipt_passed'])
        self.assertIsNotNone(session.pending)
        self.assertEqual(session.report['manual_clicks'], 1)

    def test_pending_favorite_blocks_scroll(self):
        self.backend.replies = [dynamic_packet()]
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'COLLECTION_PENDING_RECONCILIATION'):
            with session:
                session.perform(fixtures.capture(collection_layout=True))
                plan = scroll_plan(session.previous, rule(), [])
                session.perform(fixtures.collect())
                session.perform(plan['steps'][0])
        self.assertFalse(any(isinstance(event, tuple) and event[0] == 'scroll' for event in self.backend.events))

    def test_dynamic_cli_flags_and_independent_profile_flag(self):
        self.backend.replies = [fixtures.packet(), dynamic_packet(), dynamic_packet('b')]
        session = self.session()
        with session:
            # Profile does not require a ready dynamic detector contract.
            session.perform(fixtures.capture(collection_layout_profile=True))
            session.perform(fixtures.capture(collection_layout=True, card_id='observed-row'))
            session.perform(fixtures.capture(collection_layout=True, visible_index=0))
        commands = [e[1] for e in self.backend.events if isinstance(e, tuple) and e[0] == 'capture']
        self.assertIn('--collection-layout-profile', commands[0])
        self.assertNotIn('--collection-layout', commands[0])
        self.assertIn('--collection-card-id', commands[1])
        self.assertIn('--collection-visible-index', commands[2])

    def test_dynamic_selector_conflicts_rejected_before_diagnostic(self):
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'COLLECTION_DYNAMIC_CARD_SELECTOR_CONFLICT'):
            with session:
                session.perform(fixtures.capture(collection_layout=True, card_id='row', visible_index=0))
        self.assertFalse(any(isinstance(event, tuple) and event[0] == 'capture' for event in self.backend.events))

    def test_motion_elapsed_and_points_are_logged(self):
        self.backend.motion_seconds = .25
        session = self.session()
        with session:
            session.perform(fixtures.capture())
            action = session.perform(dict(kind='click', expected_before='skin_listings', point=[500, 420]))
        self.assertEqual(action['timings']['motion_ms'], 250)
        self.assertEqual(action['motion']['planned_points'], 5)
        self.assertEqual(action['motion']['moved_points'], 4)

    def test_motion_expiry_blocks_favorite_after_prepare(self):
        self.backend.motion_seconds = 5
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'COLLECTION_OBSERVATION_EXPIRED'):
            with session:
                session.perform(fixtures.capture())
                session.perform(fixtures.collect())
        self.assertFalse(any(isinstance(event, tuple) and event[0] == 'click' for event in self.backend.events))
        self.assertIsNotNone(session.pending)
        document = json.loads(next(session.journal.directory.glob('*.json')).read_text(encoding='utf-8'))
        self.assertEqual(document['status'], 'prepared')
        self.assertEqual(session.steps[-1]['timings']['motion_ms'], 5000)

    def test_motion_expiry_also_blocks_navigation(self):
        self.backend.motion_seconds = 5
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'COLLECTION_OBSERVATION_EXPIRED'):
            with session:
                session.perform(fixtures.capture())
                session.perform(dict(kind='click', expected_before='skin_listings', point=[168, 1403]))
        self.assertEqual(session.report['manual_clicks'], 0)

    def test_motion_interruption_never_dispatches(self):
        def interrupted(point, before_dispatch=None):
            self.backend.last_motion = dict(completed=False, elapsed_ms=85, moved_points=3, error='CURSOR_INTERFERENCE')
            raise MotionInterrupted('CURSOR_INTERFERENCE', self.backend.last_motion)
        self.backend.click = interrupted
        session = self.session()
        with self.assertRaisesRegex(MotionInterrupted, 'CURSOR_INTERFERENCE'):
            with session:
                session.perform(fixtures.capture())
                session.perform(dict(kind='click', expected_before='skin_listings', point=[500, 420]))
        self.assertEqual(session.report['manual_clicks'], 0)
        self.assertFalse(session.steps[-1]['motion']['completed'])
        self.assertTrue(session.report['ide_restored'])

    def test_snapshot_change_during_motion_stops_before_star(self):
        original = self.backend.click
        def changed(point, before_dispatch=None):
            data = fixtures.snapshot()
            data['rows'][0]['price_max'] = '1000'
            self.snapshot_path.write_text(json.dumps(data), encoding='utf-8')
            return original(point, before_dispatch=before_dispatch)
        self.backend.click = changed
        session = self.session()
        with self.assertRaisesRegex(RuntimeError, 'COLLECTION_SNAPSHOT_CHANGED'):
            with session:
                session.perform(fixtures.capture())
                session.perform(fixtures.collect())
        self.assertEqual(session.report['manual_clicks'], 0)


if __name__ == '__main__':
    unittest.main(verbosity=2)
