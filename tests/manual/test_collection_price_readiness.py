"""Recorded low-confidence price is retry-only; no threshold or input bypass."""
import copy
import json
from pathlib import Path
import tempfile
import unittest

from collection_live_session import ForegroundSession, local_price_readiness_waiting
from collection_scroll import selection_plan
from test_collection_live_session import Clock, FakeBackend, capture


ROOT=Path(__file__).resolve().parents[2]

def fresh(value,suffix):
    result=copy.deepcopy(value)
    fid,digest='synthetic:price-readiness:'+suffix,suffix*64
    for key in ('collection_observation','collection_layout','collection_selected_card'):
        result[key].update(frame_id=fid,frame_sha256=digest)
    for key in ('local_price_ocr','local_title_ocr'):
        result[key].update(source_frame_id=fid,source_frame_sha256=digest)
    result['frames'][-1]['sha256']=digest
    return result

class PriceReadinessTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        path=ROOT/'artifacts/collection_scroll_speed/white_star_run_06/session.json'
        if not path.is_file():raise unittest.SkipTest('local run06 absent')
        session=json.loads(path.read_text('utf-8'))
        cls.pending=session['pending_geometry']
        cls.before=session['steps'][-3]['result']
        cls.after=session['steps'][-1]['result']

    def test_real_low_confidence_only_requests_another_read(self):
        frozen=copy.deepcopy((self.after,self.pending))
        evidence=local_price_readiness_waiting(self.after,self.pending)
        self.assertEqual(evidence['reason'],'local_price_confidence_pending')
        self.assertFalse(evidence['collection_allowed'])
        self.assertFalse(evidence['price_accepted'])
        self.assertEqual(evidence['minimum_confidence_unchanged'],.99)
        self.assertEqual((self.after,self.pending),frozen)

    def test_invalid_source_score_or_geometry_never_enables_retry(self):
        for name in ('title','frame','score_nan','score_bool','score_high','text','selected','geometry','error'):
            value=copy.deepcopy(self.after)
            if name=='title':value['local_title_ocr']['ok']=False
            elif name=='frame':value['local_price_ocr']['source_frame_id']='stale'
            elif name.startswith('score_'):value['local_price_ocr']['scores']=[{'score_nan':float('nan'),'score_bool':True,'score_high':1.}[name]]
            elif name=='text':value['local_price_ocr']['raw_texts']=['3OO']
            elif name=='selected':value['collection_layout']['cards'][2]['selected']=False
            elif name=='geometry':value['collection_layout']['cards'][2]['bounds'][1]-=10
            else:value['local_title_error']='LOCAL_PRICE_CARD_BINDING'
            with self.subTest(name=name):self.assertIsNone(local_price_readiness_waiting(value,self.pending))

    def run_fake(self,replies):
        temp=tempfile.TemporaryDirectory();self.addCleanup(temp.cleanup)
        clock=Clock();backend=FakeBackend(clock,replies=[copy.deepcopy(self.before),*copy.deepcopy(replies)])
        session=ForegroundSession('artifacts/price-readiness/session.json',root=Path(temp.name),backend=backend,
            now=clock.now,wait=clock.wait,fast_settle=True)
        def run():
            with session:
                session.perform(capture(collection_layout=True))
                for step in selection_plan(session.previous,self.pending['rule'],self.pending['lease']['card']['id'])['steps']:
                    session.perform(step)
        return session,run

    def test_three_low_confidence_frames_stop_with_original_pending_and_no_stars(self):
        session,run=self.run_fake([self.after,fresh(self.after,'b'),fresh(self.after,'c')])
        with self.assertRaisesRegex(RuntimeError,'BATCH_STEP_FAILED'):run()
        self.assertEqual(session.steps[-1]['attempt_count'],3)
        self.assertEqual(session.report['manual_clicks'],1)
        self.assertIsNotNone(session.pending_geometry)
        self.assertIsNone(session.selected_lease)
        self.assertIsNone(session.pending)
        self.assertEqual(list(session.journal.directory.glob('*.json')),[])
        self.assertTrue(session.report['ide_restored'])

    def test_duplicate_frame_stops_without_third_read(self):
        session,run=self.run_fake([self.after,self.after])
        with self.assertRaisesRegex(RuntimeError,'BATCH_STEP_FAILED'):run()
        self.assertEqual(session.steps[-1]['attempt_count'],2)
        self.assertFalse(session.steps[-1]['collection_price_retryable'])
        self.assertEqual(session.report['manual_clicks'],1)

    def test_independently_passing_new_capture_still_uses_normal_selection_rebind(self):
        passed=fresh(self.after,'b')
        passed['capture_passed']=True;passed.pop('local_title_error')
        passed['local_price_ocr'].update(ok=True,error='',scores=[.999])
        session,run=self.run_fake([self.after,passed]);run()
        self.assertEqual(session.steps[-1]['attempt_count'],2)
        self.assertIsNone(session.pending_geometry)
        self.assertIsNotNone(session.selected_lease)
        self.assertEqual(session.report['manual_clicks'],1)
        self.assertEqual(list(session.journal.directory.glob('*.json')),[])

if __name__=='__main__':unittest.main()
