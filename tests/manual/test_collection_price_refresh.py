"""Recorded receipt-only refresh: fresh price retry, no input or relaxed score."""
import copy
import json
from pathlib import Path
import tempfile
import unittest

from collection_live_session import ForegroundSession,local_price_refresh_waiting
from test_collection_live_session import Clock,FakeBackend,capture


FIXTURE=Path(__file__).resolve().parents[1]/'fixtures/collection_price_refresh_run04.json'


def fresh(packet,suffix):
    p=copy.deepcopy(packet);fid='synthetic:refresh:'+suffix;digest=suffix*64
    for k in ('collection_observation','collection_layout','collection_selected_card'):
        p[k].update(frame_id=fid,frame_sha256=digest)
    for k in ('local_title_ocr','local_price_ocr'):
        p[k].update(source_frame_id=fid,source_frame_sha256=digest)
    p['frames'][-1]['sha256']=digest
    return p


class PriceRefreshTests(unittest.TestCase):
    def setUp(self):
        self.data=json.loads(FIXTURE.read_text('utf8'))
        self.origin,self.failed,self.good=(self.data[k] for k in ('origin','failed','fresh'))

    def test_actual_failed_then_independent_high_confidence_frame(self):
        before=copy.deepcopy(self.data)
        evidence=local_price_refresh_waiting(self.failed,self.origin)
        self.assertIsNotNone(evidence)
        self.assertFalse(evidence['price_accepted'])
        self.assertFalse(evidence['collection_allowed'])
        self.assertFalse(evidence['click_retry_allowed'])
        self.assertEqual(evidence['minimum_confidence_unchanged'],.99)
        self.assertEqual(self.failed['local_price_ocr']['scores'],[.88659])
        self.assertEqual(self.good['local_price_ocr']['scores'],[.99998])
        self.assertIsNone(local_price_refresh_waiting(self.good,self.origin))
        self.assertEqual(before,self.data)

    def test_receipt_origin_and_current_same_frame_gold_required(self):
        for kind in ('unconfirmed','full','origin_stale','current_stale','wrong_title','moved','white','bad_score','wrong_error'):
            origin=copy.deepcopy(self.origin);failed=copy.deepcopy(self.failed)
            if kind=='unconfirmed':origin['collection_receipt_passed']=False
            elif kind=='full':origin['collection_geometry_scope']='full'
            elif kind=='origin_stale':origin['local_title_ocr']['source_frame_id']='old'
            elif kind=='current_stale':failed['local_price_ocr']['source_frame_id']='old'
            elif kind=='wrong_title':failed['local_title_ocr']['words'][0]['text']='wrong product'
            elif kind=='moved':failed['collection_selected_card']['bounds'][1]+=10
            elif kind=='white':failed['collection_selected_card']['favorite_warm_fraction']=0
            elif kind=='bad_score':failed['local_price_ocr']['scores']=[True]
            else:failed['local_title_error']='LOCAL_PRICE_CARD_BINDING'
            with self.subTest(kind=kind):
                self.assertIsNone(local_price_refresh_waiting(failed,origin))

    def run_fake(self,replies):
        temp=tempfile.TemporaryDirectory();self.addCleanup(temp.cleanup)
        clock=Clock();backend=FakeBackend(clock,copy.deepcopy(replies))
        session=ForegroundSession('artifacts/refresh/session.json',root=Path(temp.name),backend=backend,
            now=clock.now,wait=clock.wait,fast_settle=True,numeric_price=True)
        def run():
            with session:
                session.previous=copy.deepcopy(self.origin)
                session.previous_received=clock.now();session.previous_age_upper_ms=200
                return session.perform(capture(collection_layout=True))
        return session,backend,run

    def test_bounded_fresh_read_succeeds_without_any_click(self):
        session,backend,run=self.run_fake([self.failed,self.good]);step=run()
        self.assertTrue(step['passed']);self.assertEqual(step['attempt_count'],2)
        self.assertTrue(step['attempts'][0]['collection_price_retryable'])
        self.assertTrue(step['result']['collection_layout_passed'])
        self.assertEqual(step['result']['local_price_ocr']['scores'],[.99998])
        self.assertEqual(session.report['manual_clicks'],0)
        self.assertEqual(list(session.journal.directory.glob('*.json')),[])
        self.assertTrue(session.report['ide_restored'])

    def test_three_failed_frames_stop_without_accepting_old_price(self):
        session,backend,run=self.run_fake([self.failed,fresh(self.failed,'b'),fresh(self.failed,'c')])
        with self.assertRaisesRegex(RuntimeError,'BATCH_STEP_FAILED'):run()
        self.assertEqual(session.steps[-1]['attempt_count'],3)
        self.assertFalse(session.steps[-1]['result']['capture_passed'])
        self.assertEqual(session.report['manual_clicks'],0)
        self.assertEqual(list(session.journal.directory.glob('*.json')),[])

    def test_duplicate_failed_frame_ends_retry(self):
        session,backend,run=self.run_fake([self.failed,self.failed,self.good])
        with self.assertRaisesRegex(RuntimeError,'BATCH_STEP_FAILED'):run()
        self.assertEqual(session.steps[-1]['attempt_count'],2)
        self.assertFalse(session.steps[-1]['collection_price_retryable'])
        self.assertEqual(len(backend.replies),1)


if __name__=='__main__':unittest.main()
