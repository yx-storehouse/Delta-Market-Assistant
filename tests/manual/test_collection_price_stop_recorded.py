"""Replay recorded OCR fields through the price stop; never operate the game.

The four-window scroll probe deliberately continued past price 230. These tests
keep that diagnostic separate from the real collect_selected / scan_row paths.
Navigation and backend I/O are fake; the recorded price and configured rule
are unchanged. The original game images are neither loaded nor written.
"""
import copy
import json
from pathlib import Path
import tempfile
import unittest

from collection_candidate import match_selected_card
from collection_live_session import ForegroundSession
from collection_scroll import observe_layout
from run_collection_trial import CollectionTrial
from test_collection_live_session import Clock, FakeBackend, capture


ROOT = Path(__file__).resolve().parents[2]
TX = ROOT / 'artifacts/collection_scroll_speed'


class ReplayTrial(CollectionTrial):
    """Only navigation is omitted: selection, matching and price stop are real."""
    def open_product(self, row):
        pass

    def condition(self, row):
        pass


class RecordedPriceStopTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        paths = (TX / 'input/task_snapshot.json', TX / 'windows_03/session.json',
                 TX / 'windows_03/result.json')
        if not all(path.is_file() for path in paths):
            raise unittest.SkipTest('local recorded scroll module test is absent')
        cls.snapshot, cls.recorded, cls.probe = [json.loads(p.read_text('utf-8')) for p in paths]
        cls.rule = next(row for row in cls.snapshot['rows'] if row['row_index'] == 7)
        cls.packet = None
        for step in cls.recorded['steps']:
            if step.get('kind') != 'capture' or not step.get('passed'):
                continue
            packet = step.get('result', {})
            try:
                candidate = match_selected_card(packet, cls.rule)
            except (ValueError, KeyError):
                continue
            if candidate['price'] == '300':
                cls.packet = copy.deepcopy(packet)
                break
        if cls.packet is None:
            raise AssertionError('expected original price-300 OCR packet missing')

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.path = self.root / 'artifacts/replay/snapshot.json'
        self.path.parent.mkdir(parents=True)
        self.path.write_text(json.dumps(self.snapshot, ensure_ascii=False), 'utf-8')
        self.clock = Clock()
        self.backend = FakeBackend(self.clock, replies=[copy.deepcopy(self.packet)])
        self.session = ForegroundSession('artifacts/replay/session.json', root=self.root,
            backend=self.backend, now=self.clock.now, wait=self.clock.wait)

    def assert_no_input_or_collection(self):
        self.assertFalse([event for event in self.backend.events
                          if isinstance(event, tuple) and event[0] in ('click', 'scroll')])
        self.assertEqual(list(self.session.journal.directory.glob('*.json')), [])
        self.assertIsNone(self.session.pending)
        self.assertIsNone(self.session.pending_geometry)

    def test_recorded_probe_was_not_a_collection_run(self):
        self.assertEqual(self.rule['price_max'], '230')
        self.assertEqual(self.probe['mode'], 'read_only_module_test')
        self.assertEqual(len(self.probe['windows']), 4)
        self.assertEqual(sum(len(w['readings']) for w in self.probe['windows']), 20)
        self.assertFalse(any(step['kind'] == 'collect_selected' for step in self.recorded['steps']))
        self.assertTrue(all(reading['candidate']['eligible'] is False
                            for window in self.probe['windows'] for reading in window['readings']))
        self.assertEqual(self.probe['star_actions'], 0)
        self.assertFalse(self.probe['collection_completion_claimed'])

    def test_actual_collection_gate_skips_future_input_after_price_300_above_230(self):
        with self.session:
            self.session.perform(capture(collection_layout=True))
            action = self.session.perform(dict(kind='collect_selected', expected_before='skin_listings',
                snapshot='artifacts/replay/snapshot.json', row_index=7, viewport=[2560, 1440],
                skip_ineligible=True, skip_favorited=True, end_segment_on_price_above=True))
            self.assertEqual(action['candidate']['price'], '300')
            self.assertFalse(action['candidate']['eligible'])
            self.assertTrue(self.session.segment_finished)
            for step in (dict(kind='scroll', point=[1000, 700], delta=-120),
                         dict(kind='click', point=[550, 680])):
                result = self.session.perform(dict(step, expected_before='skin_listings'))
                self.assertTrue(result['skipped'])
                self.assertEqual(result['reason'], 'price_limit_segment_finished')
        self.assert_no_input_or_collection()

    def test_actual_scanner_ends_current_rule_on_first_above_limit_card_without_scroll(self):
        single = copy.deepcopy(self.snapshot)
        single.update(rows=[copy.deepcopy(self.rule)], enabled_task_count=1, disabled_task_count=0)
        self.path.write_text(json.dumps(single, ensure_ascii=False), 'utf-8')
        with self.session:
            self.session.perform(capture(collection_layout=True))
            full = [card for card in observe_layout(self.session.previous)['cards'] if card['selectable']]
            self.assertTrue(full[0]['selected'])
            trial = ReplayTrial(self.session, single, str(self.path))
            trial.scan_row(self.rule)
        self.assertEqual(trial.summary['scanned_candidates'], 1)
        self.assertEqual(trial.summary['confirmed_new'], 0)
        self.assertEqual(len(trial.summary['rows']), 1)
        completion = trial.summary['rows'][0]
        self.assertEqual(completion['status'], 'original_price_stop_boundary')
        self.assertEqual(completion['boundary_candidate']['price'], '300')
        self.assertEqual(completion['visible_windows'], 1)
        self.assertFalse(completion['unobserved_tail_exhaustive'])
        self.assertEqual([step['kind'] for step in self.session.steps], ['capture', 'collect_selected'])
        self.assert_no_input_or_collection()


if __name__ == '__main__':
    unittest.main()
