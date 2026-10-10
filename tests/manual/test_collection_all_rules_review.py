"""Independent pure review of the dynamic coordinator; no game or filesystem I/O.

The fake session executes the real geometry bind/validate/rebind functions.
Only OCR/OS responses are constructed; no native capture or cursor is imported.
"""
import copy
from decimal import Decimal
from types import SimpleNamespace
import unittest

from collection_scroll import validate_card_lease, validate_scroll_lease, rebind_selected, rebind_after_scroll
from run_collection_trial import CollectionTrial
from test_collection_scroll import packet as geometry_packet, card as geometry_card


def rule(index=0, product='AUG突击步枪-天命', enabled=True):
    return dict(row_index=index, task_id=f'current-task-{index}', enabled=enabled,
        dictionary_resolved=True, product_name=product, season_label='棱镜攻势 S2',
        condition_label='成色S', price_min='10', price_max='600', max_wear='5', limit_raw=0)


def snapshot(rows):
    return dict(schema='collection-run-snapshot-v1', source_kind='current_config',
        ready=True, mode='collect_only', purchase_phase_enabled=False,
        source_sha256='a' * 64, config_sha256='a' * 64, rows=rows)


class EmptyJournal:
    def glob(self, pattern):
        return []


class GeometrySession:
    def __init__(self, candidates_by_row, *, initially_selected=0, initial_window=0):
        self.by_row = copy.deepcopy(candidates_by_row)
        self.initially_selected = initially_selected
        self.initial_window = initial_window
        self.rule = rule()
        self.index = initially_selected
        self.window = initial_window
        self.serial = 0
        self.previous = {}
        self.segment_finished = False
        self.pending = self.pending_geometry = None
        self.steps = []
        self.collected_rows = []
        self.selection_leases = []
        self.scroll_leases = []
        self.remaining = 900
        self.receipt_failure = False
        self.omit_scroll_rebind = False
        self.selection_geometry_offset_px = 0
        self.selection_geometry_offset_y_px = 0
        self.bottom = False
        self.journal = SimpleNamespace(directory=EmptyJournal())
        self.refresh()

    def refresh(self):
        self.serial += 1
        packet = geometry_packet()
        digest = f'{self.serial:064x}'
        packet['frames'][0]['sha256'] = digest
        for name in ('collection_layout', 'collection_observation'):
            packet[name]['frame_id'] = f'review-frame:{self.serial}'
            packet[name]['frame_sha256'] = digest
        layout = packet['collection_layout']
        if self.window:
            # Non-grid offsets and different card count force true rebinding.
            layout['cards'] = [geometry_card(f'new-{index}', 120 + (index % 2) * 877,
                341 + (index // 2) * 289) for index in range(4)]
        for index, card in enumerate(layout['cards']):
            card['id'] = f'frame-{self.serial}-card-{index}'
            card['selected'] = index == self.index
        layout['scrollbar']['thumb_bounds'][1] += self.window * 40
        if self.bottom:
            track, thumb = layout['scrollbar']['track_bounds'], layout['scrollbar']['thumb_bounds']
            thumb[1] = track[1] + track[3] - thumb[3]
        packet['collection_observation']['regions'][0]['words'][0]['text'] = self.rule['product_name']
        self.previous = packet

    def begin(self, current_rule):
        self.rule = current_rule
        self.index = self.initially_selected
        self.window = self.initial_window
        self.refresh()

    def remaining_seconds(self):
        return self.remaining

    def reset_segment(self):
        if self.pending is not None or self.pending_geometry is not None:
            raise RuntimeError('REVIEW_PENDING_STATE')
        self.segment_finished = False

    def perform(self, step):
        if len(self.steps) >= 500:
            raise RuntimeError('REVIEW_BOUNDED_STEP_BUDGET')
        self.steps.append(copy.deepcopy(step))
        kind = step['kind']
        if kind == 'select_visible_card':
            lease = validate_card_lease(self.previous, self.rule, step['lease'])
            self.selection_leases.append(copy.deepcopy(lease))
            self.index = next(i for i, card in enumerate(self.previous['collection_layout']['cards'])
                              if card['id'] == lease['card']['id'])
            self.pending_geometry = ('selection', lease)
            self.previous = {}
            return dict(passed=True)
        if kind == 'scroll_visible_list':
            lease = validate_scroll_lease(self.previous, self.rule, [], step['lease'])
            self.scroll_leases.append(copy.deepcopy(lease))
            self.window += 1 if lease['delta'] < 0 else -1
            self.index = None
            self.pending_geometry = ('scroll', lease)
            self.previous = {}
            return dict(passed=True)
        if kind == 'capture':
            self.refresh()
            if self.pending_geometry:
                action, lease = self.pending_geometry
                if action == 'selection':
                    if self.selection_geometry_offset_px or self.selection_geometry_offset_y_px:
                        selected = next(card for card in self.previous['collection_layout']['cards'] if card['selected'])
                        # Keep rectangles legal and nonoverlapping so a failure
                        # specifically tests the measured contour tolerance.
                        for axis, offset in enumerate((self.selection_geometry_offset_px,
                                                       self.selection_geometry_offset_y_px)):
                            selected['bounds'][axis] += offset
                            selected['bounds'][axis + 2] -= offset
                            selected['fields_bounds'][axis] += offset
                            selected['fields_bounds'][axis + 2] -= offset
                    evidence = rebind_selected(self.previous, self.rule, lease)
                else:
                    evidence = rebind_after_scroll(self.previous, self.rule, lease)
                if not (action == 'scroll' and self.omit_scroll_rebind):
                    self.previous['collection_geometry_rebind'] = dict(kind=action, passed=True, evidence=evidence)
                self.pending_geometry = None
            if step.get('expect_collection_added'):
                if self.receipt_failure:
                    raise RuntimeError('REVIEW_RECEIPT_UNPROVEN')
                if self.pending is None:
                    raise RuntimeError('REVIEW_NO_PENDING_COLLECTION')
                self.pending = None
                self.previous['collection_receipt_passed'] = True
            return dict(passed=True)
        if kind == 'collect_selected':
            selected = [c for c in self.previous['collection_layout']['cards'] if c['selected']]
            if len(selected) != 1:
                raise RuntimeError('REVIEW_SELECTED_CARD_REQUIRED')
            value = self.by_row[self.rule['row_index']].pop(0)
            price, wear = Decimal(str(value.get('price', 230))), Decimal(str(value.get('wear', '.25')))
            eligible = Decimal(self.rule['price_min']) <= price <= Decimal(self.rule['price_max']) and wear <= Decimal(self.rule['max_wear'])
            candidate = dict(product=self.rule['product_name'], condition=self.rule['condition_label'],
                price=str(price), wear=str(wear), eligible=eligible, row_index=self.rule['row_index'],
                source_frame_id=self.previous['collection_observation']['frame_id'],
                source_frame_sha256=self.previous['collection_observation']['frame_sha256'],
                card_bounds=selected[0]['bounds'])
            if not eligible:
                self.segment_finished = price > Decimal(self.rule['price_max'])
                return dict(passed=True, skipped=True, reason='rule_not_matched', candidate=candidate)
            if value.get('gold'):
                return dict(passed=True, skipped=True, reason='already_favorited', candidate=candidate)
            self.pending = candidate
            self.collected_rows.append(self.rule['row_index'])
            self.previous = {}
            return dict(passed=True, collection_attempt=candidate)
        raise AssertionError('Unexpected input kind: ' + kind)


class ReviewTrial(CollectionTrial):
    def __init__(self, session, rows):
        super().__init__(session, snapshot(rows), 'artifacts/review/input.json', snapshot_sha256='b' * 64)
        self.opened = []
        self.fail_open_row = None

    def startup(self):
        # Navigation is not the subject of these dynamic-loop tests.
        return

    def open_product(self, current_rule):
        self.opened.append(current_rule['row_index'])
        if current_rule['row_index'] == self.fail_open_row:
            raise RuntimeError('REVIEW_PRODUCT_NOT_OBSERVED')

    def condition(self, current_rule):
        self.session.reset_segment()
        self.session.begin(current_rule)


class AllRulesReviewTests(unittest.TestCase):
    def test_receipt_only_confirmation_reobserves_complete_layout_before_next_action(self):
        session = GeometrySession({0: [dict(price=230), dict(price=601)]})
        original = session.perform
        def perform(step):
            if session.previous.get('collection_geometry_scope') == 'receipt_only':
                self.assertEqual(step['kind'], 'capture')
                self.assertTrue(step['collection_layout'])
                self.assertNotIn('expect_collection_added', step)
            result = original(step)
            if step.get('expect_collection_added'):
                session.previous['collection_geometry_scope'] = 'receipt_only'
                session.previous['collection_layout']['complete'] = False
            return result
        session.perform = perform
        trial = ReviewTrial(session, [rule()])
        trial.run()
        self.assertTrue(trial.summary['task_file_fully_completed'])
        self.assertEqual(trial.summary['confirmed_new'], 1)
        receipt_index = next(i for i,s in enumerate(session.steps) if s.get('expect_collection_added'))
        self.assertEqual(session.steps[receipt_index+1]['kind'], 'capture')
        self.assertNotIn('expect_collection_added', session.steps[receipt_index+1])

    def test_receipt_confirmed_count_is_preserved_if_full_layout_followup_blocks(self):
        session = GeometrySession({0: [dict(price=230)]})
        original = session.perform
        def perform(step):
            if session.previous.get('collection_geometry_scope') == 'receipt_only':
                self.assertEqual(step['kind'], 'capture')
                raise RuntimeError('REVIEW_FULL_LAYOUT_STILL_UNPROVEN')
            result = original(step)
            if step.get('expect_collection_added'):
                session.previous['collection_geometry_scope'] = 'receipt_only'
                session.previous['collection_layout']['complete'] = False
            return result
        session.perform = perform
        trial = ReviewTrial(session, [rule()])
        with self.assertRaisesRegex(RuntimeError, 'FULL_LAYOUT_STILL_UNPROVEN'):
            trial.run()
        self.assertEqual(trial.summary['confirmed_new'], 1)
        self.assertIsNone(session.pending)
        self.assertFalse(trial.summary['task_file_fully_completed'])
        self.assertEqual(session.collected_rows, [0])

    def test_every_enabled_rule_runs_real_dynamic_loop_not_mock_scan_row(self):
        enabled = set(range(0, 42, 2))
        rows = [rule(i, f'configured-product-{(i // 2) % 9}', i in enabled) for i in range(50)]
        values = {i: [dict(price=230), dict(price=601)] for i in enabled}
        session = GeometrySession(values)
        trial = ReviewTrial(session, rows)
        trial.run()
        self.assertEqual(trial.opened, sorted(enabled))
        self.assertEqual(session.collected_rows, sorted(enabled))
        self.assertEqual(len({row['product_name'] for row in rows if row['enabled']}), 9)
        self.assertEqual(trial.summary['confirmed_new'], 21)
        self.assertEqual([row['row_index'] for row in trial.summary['rows']], sorted(enabled))
        self.assertTrue(trial.summary['task_file_fully_completed'])
        self.assertFalse(trial.summary['exhaustive_market_scan'])
        self.assertTrue(all(row['unobserved_tail_exhaustive'] is False for row in trial.summary['rows']))

    def test_failure_does_not_dispatch_later_rules_or_claim_complete(self):
        rows = [rule(i) for i in range(5)]
        trial = ReviewTrial(GeometrySession({i: [dict(price=601)] for i in range(5)}), rows)
        trial.fail_open_row = 2
        with self.assertRaisesRegex(RuntimeError, 'PRODUCT_NOT_OBSERVED'):
            trial.run()
        self.assertEqual(trial.opened, [0, 1, 2])
        self.assertEqual([r['row_index'] for r in trial.summary['rows']], [0, 1])
        self.assertFalse(trial.summary['task_file_fully_completed'])

    def test_selected_first_card_is_used_without_reclick(self):
        session = GeometrySession({0: [dict(price=601)]}, initially_selected=0)
        ReviewTrial(session, [rule()]).run()
        self.assertEqual([s['kind'] for s in session.steps], ['collect_selected'])

    def test_unselected_first_card_is_selected_and_rebound_before_match(self):
        session = GeometrySession({0: [dict(price=601)]}, initially_selected=None)
        ReviewTrial(session, [rule()]).run()
        self.assertEqual([s['kind'] for s in session.steps], ['select_visible_card', 'capture', 'collect_selected'])
        lease = session.selection_leases[0]
        self.assertNotEqual(lease['frame_id'], session.previous['collection_observation']['frame_id'])

    def test_different_selected_card_is_not_mistaken_for_first_card(self):
        session = GeometrySession({0: [dict(price=601)]}, initially_selected=4)
        ReviewTrial(session, [rule()]).run()
        self.assertEqual(session.selection_leases[0]['card']['bounds'][:2], [120, 303])
        self.assertEqual(session.index, 0)

    def test_scroll_rebind_uses_new_nongrid_rectangles_and_frame_local_ids(self):
        session = GeometrySession({0: [dict(price=230, gold=True)] * 6 + [dict(price=601)]})
        trial = ReviewTrial(session, [rule()])
        trial.run()
        self.assertEqual(trial.summary['scanned_candidates'], 7)
        self.assertEqual(trial.summary['rows'][0]['visible_windows'], 2)
        self.assertEqual(len(session.scroll_leases), 1)
        self.assertEqual(session.selection_leases[-1]['card']['bounds'][:2], [120, 341])
        self.assertNotEqual(session.selection_leases[-1]['frame_id'], session.scroll_leases[0]['frame_id'])
        self.assertTrue(all('card_index' not in step and step['kind'] != 'click' for step in session.steps))

    def test_missing_scroll_rebind_stops_before_any_new_window_collection(self):
        session = GeometrySession({0: [dict(price=230, gold=True)] * 6 + [dict(price=601)]})
        session.omit_scroll_rebind = True
        trial = ReviewTrial(session, [rule()])
        with self.assertRaisesRegex(RuntimeError, 'SCROLL_REBIND_REQUIRED'):
            trial.run()
        self.assertEqual(trial.summary['scanned_candidates'], 6)
        self.assertFalse(trial.summary['task_file_fully_completed'])

    def test_changed_selection_geometry_blocks_collection(self):
        session = GeometrySession({0: [dict(price=230)]}, initially_selected=None)
        session.selection_geometry_offset_px = 9
        trial = ReviewTrial(session, [rule()])
        with self.assertRaisesRegex(ValueError, 'SELECTION_GEOMETRY_CHANGED'):
            trial.run()
        self.assertNotIn('collect_selected', [step['kind'] for step in session.steps])
        self.assertFalse(trial.summary['task_file_fully_completed'])

    def test_one_pixel_contour_change_rebinds_and_collects(self):
        session = GeometrySession({0: [dict(price=230), dict(price=601)]}, initially_selected=None)
        session.selection_geometry_offset_px = 1
        trial = ReviewTrial(session, [rule()])
        trial.run()
        self.assertEqual(trial.summary['confirmed_new'], 1)
        self.assertEqual(trial.summary['scanned_candidates'], 2)
        self.assertEqual(len(session.selection_leases), 2)
        self.assertTrue(trial.summary['task_file_fully_completed'])
        self.assertFalse(trial.summary['exhaustive_market_scan'])
        self.assertIn('collect_selected', [step['kind'] for step in session.steps])

    def test_six_pixel_horizontal_contour_change_rebinds_and_collects(self):
        session = GeometrySession({0: [dict(price=230), dict(price=601)]}, initially_selected=None)
        session.selection_geometry_offset_px = 6
        trial = ReviewTrial(session, [rule()])
        trial.run()
        self.assertEqual(trial.summary['confirmed_new'], 1)
        self.assertEqual(trial.summary['scanned_candidates'], 2)
        self.assertEqual(len(session.selection_leases), 2)
        self.assertTrue(trial.summary['task_file_fully_completed'])
        self.assertFalse(trial.summary['exhaustive_market_scan'])

    def test_four_pixel_vertical_contour_change_blocks_collection(self):
        session = GeometrySession({0: [dict(price=230)]}, initially_selected=None)
        session.selection_geometry_offset_y_px = 4
        trial = ReviewTrial(session, [rule()])
        with self.assertRaisesRegex(ValueError, 'SELECTION_GEOMETRY_CHANGED'):
            trial.run()
        self.assertNotIn('collect_selected', [step['kind'] for step in session.steps])
        self.assertFalse(trial.summary['task_file_fully_completed'])

    def test_receipt_failure_never_increments_confirmed_or_completes(self):
        session = GeometrySession({0: [dict(price=230), dict(price=601)]})
        session.receipt_failure = True
        trial = ReviewTrial(session, [rule()])
        with self.assertRaisesRegex(RuntimeError, 'RECEIPT_UNPROVEN'):
            trial.run()
        self.assertEqual(trial.summary['confirmed_new'], 0)
        self.assertIsNotNone(session.pending)
        self.assertFalse(trial.summary['task_file_fully_completed'])

    def test_equality_is_collected_not_high_price_boundary(self):
        session = GeometrySession({0: [dict(price=600), dict(price=601)]})
        trial = ReviewTrial(session, [rule()])
        trial.run()
        self.assertEqual(trial.summary['confirmed_new'], 1)
        self.assertEqual(trial.summary['scanned_candidates'], 2)

    def test_retained_scroll_offset_is_returned_to_observed_top(self):
        session = GeometrySession({0: [dict(price=601)]}, initial_window=2)
        ReviewTrial(session, [rule()]).run()
        self.assertEqual([lease['delta'] for lease in session.scroll_leases], [120, 120])
        kinds = [step['kind'] for step in session.steps]
        self.assertEqual(kinds[:4], ['scroll_visible_list', 'capture', 'scroll_visible_list', 'capture'])
        self.assertEqual(session.window, 0)

    def test_observed_bottom_is_not_invented_page_or_task_completion(self):
        session = GeometrySession({0: [dict(price=230, gold=True)] * 6})
        trial = ReviewTrial(session, [rule()])
        # Preserve real loop/geometry checks; only bypass the top-navigation
        # phase so this test can isolate an already-observed bottom viewport.
        def at_bottom(current_rule):
            session.bottom = True
            session.refresh()
        trial.ensure_list_top = at_bottom
        with self.assertRaisesRegex(RuntimeError, 'BOTTOM_REQUIRES_PAGE_END_EVIDENCE'):
            trial.run()
        self.assertEqual(trial.summary['scanned_candidates'], 6)
        self.assertEqual(trial.summary['rows'], [])
        self.assertFalse(trial.summary['task_file_fully_completed'])
        self.assertFalse(trial.summary['exhaustive_market_scan'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
