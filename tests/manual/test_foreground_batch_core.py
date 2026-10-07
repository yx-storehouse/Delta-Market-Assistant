import unittest
from foreground_batch_core import execute_batch, stabilize_capture, filter_expectation_matches
import copy
import time


class BatchTests(unittest.TestCase):
    def test_larger_coherent_calibration_still_one_bounded_lease(self):
        events=[]
        r=execute_batch([{}]*20,lambda:events.append('enter'),lambda s,t:{'passed':True},lambda:events.append('leave') or True,lambda:True)
        self.assertTrue(r['passed']);self.assertEqual(events,['enter','leave'])
        with self.assertRaises(ValueError):execute_batch([{}]*21,lambda:None,lambda s,t:None,lambda:True,lambda:True)
        with self.assertRaises(ValueError):execute_batch([{}],lambda:None,lambda s,t:None,lambda:True,lambda:True,timeout=31)
    def test_filter_expectation_requires_complete_bound_state(self):
        result={'frames':[{'sha256':'a'*64}],'catalog_filter_state':{'valid_page':True,'complete':True,
            'same_frame':True,'frame_id':'test:frame','frame_sha256':'a'*64,'season_label':'全部赛季','checkboxes':{'owned':{'state':'unchecked'}}}}
        expected={'season':'全部赛季','owned':'unchecked'}
        self.assertTrue(filter_expectation_matches(result,expected))
        for key,value in [('complete',False),('complete','false'),('valid_page',False),('same_frame',False),('frame_id',''),('frame_sha256','other'),('frame_sha256',None)]:
            bad=copy.deepcopy(result);bad['catalog_filter_state'][key]=value
            self.assertFalse(filter_expectation_matches(bad,expected))
        for expected in [{},{'owned':'unknown'},{'unknown':'unchecked'},{'unowned':'unchecked'},{'season':'different'}]:
            self.assertFalse(filter_expectation_matches(result,expected))
        result['catalog_filter_state']['checkboxes']['owned']['state']='unknown'
        self.assertFalse(filter_expectation_matches(result,{'owned':'unchecked'}))
    def test_unknown_retry_keeps_all_observations(self):
        answers=[{'passed':False,'exit_status':1,'result':{'page_error':'E_DIAGNOSTIC_PAGE_MISMATCH','startup_page':{'page':'unknown'}}},
            {'passed':True,'exit_status':0,'result':{'startup_page':{'page':'empty_watchlist'}}}]
        r=stabilize_capture(lambda t:answers.pop(0),lambda:True,deadline=time.monotonic()+3,wait=lambda t:None)
        self.assertTrue(r['passed']);self.assertEqual(r['attempt_count'],2);self.assertFalse(r['attempts'][0]['passed'])
    def test_known_wrong_page_is_not_retried(self):
        calls=[]
        r=stabilize_capture(lambda t:calls.append(1) or {'passed':False,'exit_status':1,'result':{'page_error':'E_DIAGNOSTIC_PAGE_MISMATCH','startup_page':{'page':'skin_listings'}}},
            lambda:True,deadline=time.monotonic()+3,wait=lambda t:None)
        self.assertEqual(len(calls),1);self.assertFalse(r['passed'])
    def test_unknown_retry_is_bounded(self):
        r=stabilize_capture(lambda t:{'passed':False,'exit_status':1,'result':{'page_error':'E_DIAGNOSTIC_PAGE_MISMATCH','startup_page':{'page':'unknown'}}},
            lambda:True,deadline=time.monotonic()+3,wait=lambda t:None)
        self.assertEqual(r['attempt_count'],3);self.assertFalse(r['passed'])
    def test_retry_does_not_reclaim_lost_focus(self):
        with self.assertRaisesRegex(RuntimeError,'BATCH_FOREGROUND_LOST'):
            stabilize_capture(lambda t:self.fail('capture must not run'),lambda:False,deadline=time.monotonic()+3)
    def run_case(self, fail_at=None, lose_at=None):
        events=[]; focus=[False]; index=[0]
        def enter(): events.append('enter');focus[0]=True
        def leave(): events.append('leave');focus[0]=False;return True
        def perform(step, remaining):
            events.append('step'); index[0]+=1
            if index[0]==lose_at: focus[0]=False
            if index[0]==fail_at: raise TimeoutError('worker timeout')
            return {'passed':True}
        r=execute_batch([{}, {}, {}],enter,perform,leave,lambda:focus[0])
        return r,events
    def test_all_steps_one_lease(self):
        r,e=self.run_case();self.assertTrue(r['passed']);self.assertEqual(e,['enter','step','step','step','leave'])
    def test_failure_stops_remaining_and_restores_once(self):
        r,e=self.run_case(fail_at=2);self.assertFalse(r['passed']);self.assertEqual(e,['enter','step','step','leave']);self.assertTrue(r['ide_restored'])
    def test_user_focus_change_is_not_reacquired(self):
        r,e=self.run_case(lose_at=1);self.assertEqual(r['error'],'BATCH_FOREGROUND_LOST');self.assertEqual(e,['enter','step','leave'])
    def test_invalid_plan_does_not_touch_focus(self):
        events=[]
        with self.assertRaises(ValueError): execute_batch([],lambda:events.append(1),lambda s,t:None,lambda:events.append(2),lambda:True)
        self.assertEqual(events,[])
    def test_failed_enter_cleanup(self):
        events=[]
        def fail(): raise RuntimeError('activation failed')
        r=execute_batch([{}],fail,lambda s,t:None,lambda:events.append('leave') or True,lambda:True)
        self.assertFalse(r['passed']);self.assertEqual(events,['leave'])
    def test_failed_result_does_not_run_next_step(self):
        events=[]
        r=execute_batch([{},{}],lambda:None,lambda s,t:events.append('step') or {'passed':False},lambda:True,lambda:True)
        self.assertFalse(r['passed']);self.assertEqual(events,['step'])


if __name__=='__main__': unittest.main(verbosity=2)
