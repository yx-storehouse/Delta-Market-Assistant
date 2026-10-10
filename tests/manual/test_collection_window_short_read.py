"""A short selection frame must not end a listing window early.

Recorded (tests/fixtures/hotkey_window_short_read.json): hotkey run
2026-10-09 23:46, rule 14 AS Val突击步枪-黑银先锋 成色S 10-300, steps
#355-#376. The selection frame #374 of the last window's second card read
short (listing viewport height 548 instead of 903, scrollbar track 544
instead of 899): the second row was clipped, so the window looked done after
two of its four cards. The pipelined receipt then showed the list differed,
the runner re-read it (#376: four complete cards) and coverage refused the
scroll with COLLECTION_SCROLL_COVERAGE_INCOMPLETE after 69 favorites.
Replay only: no game, no capture, no input.
"""
import copy
import json
from pathlib import Path
from types import SimpleNamespace
import unittest

from collection_scroll import build_scroll_coverage, observe_layout, select_window_scroll_calibration
from run_collection_trial import CollectionTrial

FIXTURE = Path(__file__).resolve().parents[1] / 'fixtures' / 'hotkey_window_short_read.json'
UNSCANNED_ROW_TWO_POINT = [173, 787]  # body gutter of card [121, 743, 862, 260] in #376


class ReplayEnd(Exception):
    """The trial asked for an input beyond the recording."""


class ReplaySession:
    """Recorded packets per step; pipelined pixel receipts settle as recorded."""
    receipt_listener = None
    max_steps = None
    segment_finished = False
    pending = pending_geometry = None

    def __init__(self, recorded, unchanged_receipt_at=None):
        self.script = copy.deepcopy(recorded['steps'])
        self.previous = copy.deepcopy(recorded['first_frame'])
        self.steps, self.job = [], None
        self.unchanged_receipt_at = unchanged_receipt_at
        self.journal = SimpleNamespace(decision_records=lambda: [])

    def remaining_seconds(self):
        return 600

    def reset_segment(self):
        self.segment_finished = False

    def settle_receipts(self, settlement=None):
        if self.job is None:
            return None
        recorded, basis = self.job
        self.job = None
        if settlement is None:
            settlement = next(item for item in recorded['receipt_settlements']
                              if item['attempt_key'] == recorded['collection_attempt_key'])
        outcome = copy.deepcopy(settlement['outcome'])
        if recorded['index'] == self.unchanged_receipt_at:
            outcome['layout_unchanged'] = True
        # Session contract: an unchanged list restores the selection packet,
        # a changed one leaves no observation (a full read must follow).
        self.previous = copy.deepcopy(basis) if outcome['layout_unchanged'] else {}
        self.receipt_listener(dict(outcome, candidate=copy.deepcopy(recorded['collection_attempt'])))
        return outcome

    def perform(self, step):
        self.steps.append(copy.deepcopy(step))
        if not self.script:
            raise ReplayEnd(step['kind'], step.get('lease', {}).get('point'))
        recorded = self.script.pop(0)
        if step['kind'] != recorded['kind']:
            raise AssertionError(('diverged at', recorded['index'], recorded['kind'], step['kind']))
        if step['kind'] == 'select_visible_card':
            if step['lease']['point'] != recorded['point']:
                raise AssertionError(('select point', recorded['index'], step['lease']['point']))
            if self.job is not None:  # the dispatch guard settles the previous star first
                self.settle_receipts(recorded['receipt_settlements'][0])
            self.previous = {}
        elif step['kind'] == 'capture':
            self.previous = copy.deepcopy(recorded['result'])
        elif step['kind'] == 'collect_selected':
            # Pipelined pixel receipt: the selection packet stays the basis.
            self.job = (recorded, copy.deepcopy(self.previous))
            return dict(passed=True, **{key: copy.deepcopy(recorded[key])
                        for key in ('collection_attempt', 'collection_attempt_key', 'receipt_mode')})
        elif step['kind'] == 'scroll_visible_list':
            lease = step['lease']
            if (lease['delta'], lease['coverage']['frame_id'], lease['coverage']['layout_id']) != (
                    recorded['delta'], recorded['coverage_frame_id'], recorded['coverage_layout_id']):
                raise AssertionError(('scroll lease', recorded['index']))
            self.previous = {}
        return dict(passed=True)


class ReplayTrial(CollectionTrial):
    """Starts at the recorded first frame of the third window (#355)."""
    def open_product(self, row):
        pass

    def condition(self, row):
        pass

    def ensure_list_top(self, row):
        pass


class RecordedShortWindowTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.recorded = json.loads(FIXTURE.read_text('utf-8'))

    def packet(self, index):
        return copy.deepcopy(next(s['result'] for s in self.recorded['steps'] if s['index'] == index))

    def step(self, index):
        return copy.deepcopy(next(s for s in self.recorded['steps'] if s['index'] == index))

    def replay(self, **options):
        recorded = self.recorded
        session = ReplaySession(recorded, **options)
        snapshot = dict(ready=True, mode='collect_only', purchase_phase_enabled=False,
                        schema='collection-run-snapshot-v1', source_kind='current_config',
                        source_sha256=recorded['source_sha256'], config_sha256=recorded['config_sha256'],
                        rows=[copy.deepcopy(recorded['rule'])])
        events = []
        trial = ReplayTrial(session, snapshot, 'artifacts/replay/task_snapshot.json', emit=events.append,
                            scroll_profile_bank=copy.deepcopy(recorded['profiles']))
        with self.assertRaises(ReplayEnd) as end:
            trial.scan_row(copy.deepcopy(recorded['rule']))
        return session, trial, [event['event'] for event in events], end.exception.args

    def processed_last_window(self):
        first = observe_layout(self.packet(369))['cards']
        second = observe_layout(self.packet(371))['cards']
        cards = [next(c for c in first if c['id'] == 'edge:120:468:982:728'),
                 next(c for c in second if c['id'] == 'edge:997:468:1859:728')]
        return [dict(card=card, candidate=self.step(index)['collection_attempt'], disposition='favorite_confirmed')
                for card, index in zip(cards, (372, 375))]

    def test_recorded_selection_frame_is_short_and_hides_row_two(self):
        short, full = observe_layout(self.packet(374)), observe_layout(self.packet(376))
        self.assertEqual(short['listing_viewport'], [108, 300, 1765, 548])
        self.assertEqual(short['scrollbar']['track_bounds'], [1878, 304, 1, 544])
        self.assertEqual(full['listing_viewport'], [108, 300, 1765, 903])
        self.assertEqual(sum(c['selectable'] for c in short['cards']), 2)
        self.assertEqual(sum(c['selectable'] for c in full['cards']), 4)
        rule, processed = self.recorded['rule'], self.processed_last_window()
        # The check that fired: two processed cards against four complete ones.
        with self.assertRaisesRegex(ValueError, '^COLLECTION_SCROLL_COVERAGE_INCOMPLETE$'):
            build_scroll_coverage(self.packet(376), rule, processed)
        # On the short frame alone coverage looks complete; only the measured
        # calibration shape refuses to scroll from it.
        coverage = build_scroll_coverage(self.packet(374), rule, processed)
        with self.assertRaisesRegex(ValueError, '^COLLECTION_SCROLL_CALIBRATION_NO_SAFE_PROFILE$'):
            select_window_scroll_calibration(self.packet(374), rule, coverage, copy.deepcopy(self.recorded['profiles']))

    def test_short_selection_frame_does_not_end_the_window(self):
        session, trial, events, end = self.replay()
        # Every recorded step replays unchanged (no extra read at the normal
        # window end #367 -> #368), then the trial selects the never-scanned
        # row-two card of #376 instead of failing coverage.
        self.assertEqual([s['kind'] for s in session.steps[:-1]], [s['kind'] for s in self.recorded['steps']])
        self.assertEqual(end, ('select_visible_card', UNSCANNED_ROW_TWO_POINT))
        kinds = [s['kind'] for s in session.steps]
        self.assertEqual(kinds.count('collect_selected'), 6)   # no repeated star
        self.assertEqual(kinds.count('scroll_visible_list'), 1)  # the recorded #368 only
        self.assertEqual(trial.summary['confirmed_new'], 6)
        self.assertIn('listing_window_unscanned_after_reobserve', events)
        self.assertNotIn('listing_window_end_reobserve', events)

    def test_unchanged_receipt_against_short_basis_reads_once_more(self):
        session, trial, events, end = self.replay(unchanged_receipt_at=375)
        self.assertEqual(end, ('select_visible_card', UNSCANNED_ROW_TWO_POINT))
        self.assertEqual(events.count('listing_window_end_reobserve'), 1)
        kinds = [s['kind'] for s in session.steps]
        self.assertEqual(kinds[-3:], ['collect_selected', 'capture', 'select_visible_card'])
        self.assertEqual(kinds.count('collect_selected'), 6)
        self.assertEqual(kinds.count('scroll_visible_list'), 1)


if __name__ == '__main__':
    unittest.main()
