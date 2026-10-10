"""Current white/gold observations outrank completed statistics; pending does not.

Every action below uses a fake backend and a temporary journal. No game pixels
are captured and no OS input is dispatched.
"""
import copy
import json
from pathlib import Path
import tempfile
import unittest

from collection_journal import CollectionJournal, candidate_key, validate_record_identity
from collection_live_session import ForegroundSession
from collection_scroll import collection_disposition, scroll_plan
from reconcile_collection_attempt import pending_for_candidate
from test_collection_live_session import Clock, FakeBackend, capture, collect, packet, snapshot
from test_collection_scroll import packet as layout_packet, rule as layout_rule


class WhiteStarSessionTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.snapshot_path = self.root / 'artifacts/trial/input/snapshot.json'
        self.snapshot_path.parent.mkdir(parents=True)
        self.snapshot_path.write_text(json.dumps(snapshot()), 'utf-8')
        self.directory = self.root / 'artifacts/journal'
        self.directory.mkdir(parents=True)
        self.old_candidate = dict(product='AUG突击步枪-天命', condition='成色S', price='200',
            wear='0.187079', row_index=0, eligible=True, source_frame_id='old:before',
            source_frame_sha256='d'*64)
        self.old_record = dict(key=candidate_key(self.old_candidate), status='confirmed',
            candidate=copy.deepcopy(self.old_candidate), sent=2,
            receipt=dict(self.old_candidate, source_frame_id='old:after', source_frame_sha256='e'*64))
        self.old_path = self.directory / (self.old_record['key']+'.json')
        self.old_path.write_text(json.dumps(self.old_record, ensure_ascii=False), 'utf-8')
        self.original_bytes = self.old_path.read_bytes()
        self.clock = Clock()

    def session(self, replies, record='artifacts/trial/run.json'):
        self.backend = FakeBackend(self.clock, replies=copy.deepcopy(replies))
        return ForegroundSession(record, root=self.root,
            journal_directory=self.directory, backend=self.backend,
            now=self.clock.now, wait=self.clock.wait)

    def records(self):
        return [json.loads(path.read_text('utf-8')) for path in self.directory.glob('*.json')]

    def test_eligible_white_after_completed_history_creates_distinct_attempt_and_confirms(self):
        session = self.session([packet('a'), packet('b', gold=True)])
        with session:
            session.perform(capture())
            action = session.perform(collect(end_segment_on_price_above=True))
            new_key = action['collection_attempt_key']
            self.assertNotEqual(new_key, self.old_record['key'])
            self.assertEqual(action['collection_attempt']['price'], '230')
            self.assertEqual(action['collection_attempt']['history_disposition']['disposition'],
                             'observed_white_requires_new_attempt')
            session.perform(capture(expect_collection_added=True))
        self.assertEqual(session.report['manual_clicks'], 1)
        self.assertEqual(len(self.records()), 2)
        new = next(record for record in self.records() if record['key'] == new_key)
        self.assertEqual(validate_record_identity(new), new_key)
        self.assertEqual(new['schema'], 'collection-attempt-v2')
        self.assertEqual(new['status'], 'confirmed')
        self.assertEqual(self.old_path.read_bytes(), self.original_bytes)
        # Multiple finished attempts for the same observation composite are
        # accepted by scroll planning; they do not reintroduce the old blocker.
        self.assertEqual(scroll_plan(layout_packet(), layout_rule(), self.records())['steps'][0]['kind'],
                         'scroll_visible_list')

    def test_gold_is_preserved_without_new_attempt(self):
        session = self.session([packet('a', gold=True)])
        with session:
            session.perform(capture())
            action = session.perform(collect(end_segment_on_price_above=True))
        self.assertEqual(action['reason'], 'already_favorited')
        self.assertEqual(session.report['manual_clicks'], 0)
        self.assertEqual(len(self.records()), 1)
        self.assertEqual(self.old_path.read_bytes(), self.original_bytes)

    def test_above_max_white_still_ends_rule_without_new_attempt(self):
        config = snapshot(); config['rows'][0]['price_max'] = '220'
        self.snapshot_path.write_text(json.dumps(config), 'utf-8')
        session = self.session([packet('a')])
        with session:
            session.perform(capture())
            action = session.perform(collect(end_segment_on_price_above=True))
        self.assertEqual(action['reason'], 'rule_not_matched')
        self.assertTrue(session.segment_finished)
        self.assertEqual(session.report['manual_clicks'], 0)
        self.assertEqual(len(self.records()), 1)

    def test_dispatched_new_attempt_blocks_restart_even_though_old_attempt_is_confirmed(self):
        session = self.session([packet('a')])
        with session:
            session.perform(capture())
            session.perform(collect())
        self.assertIsNotNone(session.pending)
        self.assertEqual([r['status'] for r in self.records()].count('dispatched'), 1)
        restarted = self.session([packet('c')], 'artifacts/trial/restarted.json')
        with self.assertRaisesRegex(RuntimeError, 'COLLECTION_PENDING_RECONCILIATION'):
            with restarted:
                restarted.perform(capture())
                restarted.perform(collect())
        self.assertEqual(restarted.report['manual_clicks'], 0)
        self.assertEqual(len(self.records()), 2)
        self.assertEqual(self.old_path.read_bytes(), self.original_bytes)

    def test_uncertain_click_is_not_repeated(self):
        session = self.session([packet('a')]); self.backend.sent = 1
        with self.assertRaisesRegex(RuntimeError, 'COLLECTION_INPUT_UNCERTAIN'):
            with session:
                session.perform(capture())
                session.perform(collect())
        new = next(r for r in self.records() if r['key'] != self.old_record['key'])
        self.assertEqual(new['status'], 'input_uncertain')
        self.assertEqual(self.old_path.read_bytes(), self.original_bytes)
        with self.assertRaisesRegex(ValueError, 'COLLECTION_PENDING_RECONCILIATION'):
            collection_disposition(self.old_candidate, dict(selected=True,
                favorite_warm_fraction=0, favorite_bright_fraction=.1), self.records())


class ReconciliationAttemptTests(unittest.TestCase):
    def setUp(self):
        self.candidate = dict(product='fixture', condition='成色C', price='300', wear='1.23',
            row_index=7, source_frame_id='new:receipt', source_frame_sha256='b'*64)
        before = dict(self.candidate, source_frame_id='old:dispatch', source_frame_sha256='a'*64)
        identity = candidate_key(before)
        self.legacy = dict(key=identity, status='confirmed', candidate=before)
        self.pending = dict(schema='collection-attempt-v2', identity_key=identity,
            attempt_id='c'*32, key=identity+'.'+'c'*32, status='dispatched', candidate=before)

    def test_resolves_new_pending_attempt_not_legacy_statistics(self):
        self.assertEqual(pending_for_candidate([self.legacy, self.pending], self.candidate)['key'],
                         self.pending['key'])

    def test_completed_only_and_duplicate_pending_are_rejected(self):
        for records in ([self.legacy], [self.pending, self.pending]):
            with self.assertRaisesRegex(ValueError, 'COLLECTION_RECONCILIATION_PENDING_NOT_UNIQUE'):
                pending_for_candidate(records, self.candidate)

    def test_old_frame_is_not_a_reconciliation_receipt(self):
        with self.assertRaisesRegex(ValueError, 'COLLECTION_RECONCILIATION_FRESH_FRAME_REQUIRED'):
            pending_for_candidate([self.pending], self.pending['candidate'])


if __name__ == '__main__':
    unittest.main()
