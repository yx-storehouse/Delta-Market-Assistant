"""Replay run05's actual OCR packet without loading pixels or operating game."""
import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from collection_candidate import match_selected_first_card
from collection_live_session import ForegroundSession
from test_collection_live_session import Clock, FakeBackend, capture


ROOT = Path(__file__).resolve().parents[2]


class RecordedConditionBorderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        directory = ROOT / 'artifacts/collection_local_hotpath/run05/session.steps'
        paths = [directory / '000086.json', directory / '000087.json',
                 ROOT / 'artifacts/collection_speed/input/task_snapshot.json']
        if not all(path.is_file() for path in paths):
            raise unittest.SkipTest('local recorded run05 condition-border packet absent')
        observation, failure = [json.loads(path.read_text(encoding='utf-8')) for path in paths[:2]]
        cls.snapshot_bytes = paths[2].read_bytes()
        cls.snapshot = json.loads(cls.snapshot_bytes)
        cls.step = failure['step']
        cls.rule = next(row for row in cls.snapshot['rows'] if row['row_index'] == 3)
        cls.packet = observation['result']
        if hashlib.sha256(cls.snapshot_bytes).hexdigest() != cls.step['snapshot_sha256']:
            raise AssertionError('recorded snapshot binding changed')
        if failure['error'] != 'COLLECTION_CARD_CONDITION':
            raise AssertionError('wrong failure record')

    def test_recorded_failure_is_extra_left_edge_token_not_missing_grade_or_price(self):
        packet = self.packet
        projection = packet['collection_observation']
        self.assertEqual(projection['frame_id'], 'dxgi:ddf76d17-be91-45c4-bae0-831d5bc38bb0')
        self.assertEqual(projection['frame_sha256'],
                         'ff45d03e8615f6979224ad601fa826d7a7c9523e184d4538dc159b5d0dbc54c3')
        field = next(region for region in projection['regions'] if region['kind'] == 'card_fields')
        attempt = next(value for value in field['field_attempts'] if value['field'] == 'condition_bounds')
        self.assertEqual(attempt['tokens'], ['|', '成', '色', 'C'])
        self.assertEqual(attempt['bounds'], [120, 1072, 115, 42])
        self.assertEqual(attempt['language'], 'zh-Hans-CN')
        self.assertEqual(attempt['scale'], 2)
        self.assertEqual(field['words'][0]['x'], 120)
        self.assertAlmostEqual(field['words'][0]['width'], .46815072616725606)
        self.assertGreater(field['words'][1]['x'], 127)
        self.assertEqual(packet['local_price_ocr']['raw_texts'], ['400'])
        self.assertEqual(packet['local_price_ocr']['scores'], [.99999])
        self.assertTrue(packet['collection_geometry_rebind']['passed'])
        self.assertTrue(packet['collection_selected_card']['selected'])
        self.assertEqual(packet['collection_selected_card']['favorite_warm_fraction'], 0)

    def test_original_packet_remains_rejected_without_deleting_bar_or_borrowing_detail(self):
        packet = copy.deepcopy(self.packet)
        original = copy.deepcopy(packet)
        with self.assertRaisesRegex(ValueError, '^COLLECTION_CARD_CONDITION$'):
            match_selected_first_card(packet, self.rule)
        self.assertEqual(packet, original)

    def test_actual_session_error_sends_no_star_and_creates_no_pending_attempt(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / 'artifacts/trial/input/snapshot.json'
            path.parent.mkdir(parents=True)
            path.write_bytes(self.snapshot_bytes)
            clock = Clock()
            backend = FakeBackend(clock, replies=[copy.deepcopy(self.packet)])
            session = ForegroundSession('artifacts/replay/session.json', root=root,
                backend=backend, now=clock.now, wait=clock.wait, fast_settle=True)
            step = dict(self.step, snapshot='artifacts/trial/input/snapshot.json')
            with self.assertRaisesRegex(ValueError, '^COLLECTION_CARD_CONDITION$'):
                with session:
                    session.perform(capture(collection_layout=True))
                    session.perform(step)
            self.assertIsNone(session.pending)
            self.assertEqual(session.report['manual_clicks'], 0)
            self.assertEqual(list(session.journal.directory.glob('*.json')), [])
            self.assertFalse([event for event in backend.events
                              if isinstance(event, tuple) and event[0] in ('click', 'scroll')])
            self.assertEqual(session.report['leave_calls'], 1)
            self.assertTrue(session.report['ide_restored'])


if __name__ == '__main__':
    unittest.main()
