"""Pure orchestration regressions: no Windows backend, capture, or game input."""
import copy
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from run_collection_trial import CollectionTrial
from test_collection_scroll import packet as geometry_packet


def row():
    return dict(row_index=0, task_id='task:edited', enabled=True, dictionary_resolved=True,
                product_name='AUG突击步枪-天命', season_label='棱镜攻势 S2', condition_label='成色S',
                price_min='10', price_max='600', max_wear='5', limit_raw=0)


def snapshot():
    return dict(ready=True, mode='collect_only', purchase_phase_enabled=False,
                schema='collection-run-snapshot-v1', source_kind='current_config',
                source_sha256='a' * 64, config_sha256='a' * 64, rows=[row()])


def listing(index=0, serial=1, window=0):
    value = geometry_packet()
    digest = f'{serial:064x}'
    value['frames'][0]['sha256'] = digest
    for key in ('collection_layout', 'collection_observation'):
        value[key]['frame_sha256'] = digest
        value[key]['frame_id'] = f'fake:{serial}'
    value['collection_observation']['regions'][0]['words'][0]['text'] = row()['product_name']
    value['collection_layout']['scrollbar']['thumb_bounds'][1] += window * 40
    for i, card in enumerate(value['collection_layout']['cards']):
        card['selected'] = i == index
    value['collection_selected_card'] = {'card_id': f'c{index}', 'selected': True}
    return value


class FakeSession:
    def __init__(self, candidates=None):
        self.previous = listing()
        self.segment_finished = False
        self.pending = None
        self.steps = []
        self.candidates = list(candidates or [])
        self.remaining = 120
        self.serial = 1
        self.window = 0
        self.index = 0
        self.pending_scroll = False
        self.journal = SimpleNamespace(directory=Path('artifacts/nonexistent-pure-test-journal'))

    def remaining_seconds(self):
        return self.remaining

    def reset_segment(self):
        assert self.pending is None
        self.segment_finished = False

    def perform(self, step):
        self.steps.append(copy.deepcopy(step))
        if step['kind'] == 'capture':
            self.serial += 1
            self.previous = listing(self.index, self.serial, self.window)
            if self.pending_scroll:
                self.previous['collection_geometry_rebind'] = dict(kind='scroll', passed=True,
                    evidence=dict(scrollbar_shift_pixels=40))
                self.pending_scroll = False
            if step.get('expect_collection_added'):
                assert self.pending is not None
                self.pending = None
            return dict(passed=True)
        if step['kind'] == 'click':
            self.previous = {}
            return dict(passed=True)
        if step['kind'] == 'select_visible_card':
            self.index = int(step['lease']['card']['id'][1:])
            self.previous = {}
            return dict(passed=True)
        if step['kind'] == 'scroll_visible_list':
            self.window += 1 if step['lease']['delta'] < 0 else -1
            self.pending_scroll = True
            self.previous = {}
            return dict(passed=True)
        if step['kind'] == 'collect_selected':
            value = self.candidates.pop(0)
            candidate = dict(product=row()['product_name'], condition='成色S', price=str(value['price']),
                             wear='0.123456', row_index=0, eligible=value.get('eligible', True),
                             source_frame_id=self.previous['collection_observation']['frame_id'],
                             source_frame_sha256=self.previous['collection_observation']['frame_sha256'])
            if not candidate['eligible']:
                self.segment_finished = value['price'] > 600
                return dict(passed=True, skipped=True, reason='rule_not_matched', candidate=candidate)
            if value.get('gold'):
                return dict(passed=True, skipped=True, reason='already_favorited', candidate=candidate)
            self.pending = candidate
            self.previous = {}
            return dict(passed=True, collection_attempt=candidate)
        raise AssertionError(step)


class TestTrial(CollectionTrial):
    def open_product(self, rule):
        pass

    def condition(self, rule):
        self.session.reset_segment()
        self.session.previous = listing()


class TrialTests(unittest.TestCase):
    def test_purchase_mode_rejected_before_actions(self):
        for replacement in (dict(mode='purchase'), dict(purchase_phase_enabled=True), dict(ready=False)):
            data = snapshot()
            data.update(replacement)
            with self.assertRaises(ValueError):
                CollectionTrial(FakeSession(), data, 'artifacts/test.json')

    def test_new_favorite_requires_new_receipt_capture(self):
        session = FakeSession([{'price': 230}, {'price': 601, 'eligible': False}])
        trial = TestTrial(session, snapshot(), 'artifacts/test.json')
        trial.scan_row(row())
        self.assertEqual(trial.summary['confirmed_new'], 1)
        self.assertEqual(trial.summary['scanned_candidates'], 2)
        self.assertIsNone(session.pending)
        self.assertEqual(sum(bool(s.get('expect_collection_added')) for s in session.steps), 1)

    def test_already_favorited_is_not_new_or_toggled(self):
        session = FakeSession([{'price': 230, 'gold': True}, {'price': 601, 'eligible': False}])
        trial = TestTrial(session, snapshot(), 'artifacts/test.json')
        trial.scan_row(row())
        self.assertEqual(trial.summary['already_favorited'], 1)
        self.assertEqual(trial.summary['confirmed_new'], 0)
        self.assertFalse(any(s.get('expect_collection_added') for s in session.steps))

    def test_original_high_price_boundary_does_not_claim_exhaustive_tail(self):
        session = FakeSession([{'price': 601, 'eligible': False}])
        trial = TestTrial(session, snapshot(), 'artifacts/test.json')
        trial.scan_row(row())
        self.assertFalse(trial.summary['rows'][0]['unobserved_tail_exhaustive'])
        self.assertEqual(trial.summary['rows'][0]['status'], 'original_price_stop_boundary')
        self.assertFalse(trial.summary['task_file_fully_completed'])

    def test_below_minimum_does_not_end_segment(self):
        session = FakeSession([{'price': 9, 'eligible': False}, {'price': 601, 'eligible': False}])
        trial = TestTrial(session, snapshot(), 'artifacts/test.json')
        trial.scan_row(row())
        self.assertEqual(trial.summary['scanned_candidates'], 2)

    def test_six_cards_scroll_and_rebind_instead_of_claiming_tail_complete(self):
        session = FakeSession([{'price': 230, 'gold': True}] * 6 + [{'price': 601, 'eligible': False}])
        trial = TestTrial(session, snapshot(), 'artifacts/test.json')
        trial.scan_row(row())
        self.assertEqual(trial.summary['scanned_candidates'], 7)
        self.assertEqual(trial.summary['rows'][0]['visible_windows'], 2)
        self.assertFalse(trial.summary['rows'][0]['unobserved_tail_exhaustive'])
        self.assertEqual(sum(s['kind'] == 'scroll_visible_list' for s in session.steps), 1)
        self.assertTrue(all(s.get('collection_layout') for s in session.steps if s['kind'] == 'capture'))

    def test_no_legacy_fixed_coordinate_selection(self):
        session = FakeSession([{'price': 230, 'gold': True}, {'price': 601, 'eligible': False}])
        TestTrial(session, snapshot(), 'artifacts/test.json').scan_row(row())
        self.assertTrue(any(s['kind'] == 'select_visible_card' for s in session.steps))
        self.assertFalse(any(s['kind'] == 'click' or 'card_index' in s for s in session.steps))

    def test_missing_layout_is_not_upgraded_from_legacy_index(self):
        trial = TestTrial(FakeSession(), snapshot(), 'artifacts/test.json')
        del trial.session.previous['collection_layout']
        with self.assertRaisesRegex(ValueError, 'COLLECTION_LAYOUT_MISSING'):
            trial.ensure_list_top(row())

    def test_dispatch_without_boundary_evidence_cannot_claim_cycle_complete(self):
        data = snapshot()
        data['rows'] = [dict(row(), row_index=i, product_name=f'configured-product-{i % 9}', enabled=i < 21)
                        for i in range(50)]
        trial = TestTrial(FakeSession(), data, 'artifacts/test.json')
        visited = []
        trial.scan_row = lambda rule: visited.append(rule)
        with self.assertRaisesRegex(RuntimeError, 'COLLECTION_CYCLE_BOUNDARIES_INCOMPLETE'):
            trial.run()
        self.assertEqual([r['row_index'] for r in visited], list(range(21)))
        self.assertEqual(len({r['product_name'] for r in visited}), 9)
        self.assertFalse(trial.summary['task_file_fully_completed'])

    def test_time_budget_checked_before_collection(self):
        session = FakeSession([{'price': 230}])
        session.remaining = 11
        with self.assertRaisesRegex(RuntimeError, 'COLLECTION_TRIAL_TIME_BUDGET'):
            TestTrial(session, snapshot(), 'artifacts/test.json').scan_row(row())
        self.assertEqual(session.steps, [])

    def test_nonzero_limit_is_not_silently_unlimited(self):
        session = FakeSession()
        rule = row()
        rule['limit_raw'] = 3
        with self.assertRaisesRegex(RuntimeError, 'COLLECTION_NONZERO_LIMIT_REQUIRES_COUNTER'):
            TestTrial(session, snapshot(), 'artifacts/test.json').scan_row(rule)
        self.assertEqual(session.steps, [])

    def test_unknown_current_page_stops_without_navigation(self):
        session = FakeSession()
        def observe(step):
            session.steps.append(step)
            session.previous = {'startup_page': {'page': 'unknown', 'overlay': 'none'}}
        session.perform = observe
        with self.assertRaisesRegex(RuntimeError, 'COLLECTION_STARTUP_PAGE_NOT_CALIBRATED'):
            CollectionTrial(session, snapshot(), 'artifacts/test.json').startup()
        self.assertEqual([s['kind'] for s in session.steps], ['capture', 'capture', 'capture'])

    def test_existing_list_is_resumed_not_forced_home(self):
        session = FakeSession()
        trial = CollectionTrial(session, snapshot(), 'artifacts/test.json')
        trial.startup()
        self.assertEqual([s['kind'] for s in session.steps], ['capture'])

    def test_old_snapshot_is_not_current_user_config(self):
        data = snapshot()
        data['source_kind'] = 'savedValue'
        with self.assertRaisesRegex(ValueError, 'COLLECTION_CURRENT_CONFIG_SNAPSHOT_REQUIRED'):
            CollectionTrial(FakeSession(), data, 'artifacts/test.json')

    def test_same_title_unknown_season_requires_filter_verification(self):
        session = FakeSession()
        trial = CollectionTrial(session, snapshot(), 'artifacts/test.json')
        trial.filter_for = lambda rule: (_ for _ in ()).throw(RuntimeError('FILTER_VERIFICATION_REQUESTED'))
        with patch('run_collection_trial.exact_label_target', return_value=(100, 100)):
            with self.assertRaisesRegex(RuntimeError, 'FILTER_VERIFICATION_REQUESTED'):
                trial.open_product(row())

    def test_same_verified_season_and_title_reuses_product(self):
        session = FakeSession()
        trial = CollectionTrial(session, snapshot(), 'artifacts/test.json')
        trial.active_season = '棱镜攻势S2'
        with patch('run_collection_trial.exact_label_target', return_value=(100, 100)):
            trial.open_product(row())
        self.assertEqual(session.steps, [])

    def test_catalog_search_only_selects_observed_exact_target(self):
        session = FakeSession()
        trial = CollectionTrial(session, snapshot(), 'artifacts/test.json')
        with patch('run_collection_trial.exact_label_target', return_value=(200, 500)):
            trial.find_product(row()['product_name'])
        self.assertEqual(session.steps, [])

    def test_unreadable_catalog_does_not_blindly_scroll_or_click(self):
        session = FakeSession()
        trial = CollectionTrial(session, snapshot(), 'artifacts/test.json')
        with patch('run_collection_trial.exact_label_target', return_value=None):
            with self.assertRaisesRegex(RuntimeError, 'COLLECTION_CATALOG_NAMES_UNOBSERVED'):
                trial.find_product(row()['product_name'])
        self.assertEqual(session.steps, [])


if __name__ == '__main__':
    unittest.main()
