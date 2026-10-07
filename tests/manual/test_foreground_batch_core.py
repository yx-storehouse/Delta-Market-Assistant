import unittest
from foreground_batch_core import execute_batch


class BatchTests(unittest.TestCase):
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
