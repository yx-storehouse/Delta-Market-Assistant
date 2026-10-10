"""Real-action speed knobs remain bounded, curved and input-guarded."""
import copy
import json
from pathlib import Path
import tempfile
import unittest

from cursor_motion import plan_motion,execute_motion,MotionInterrupted
from collection_live_session import ForegroundSession
from foreground_batch_core import stabilize_capture
from test_cursor_motion import FakeCursor
import test_collection_live_session as fake


class FastCurveTests(unittest.TestCase):
    def test_fast_profile_keeps_curve_exact_endpoint_and_twelve_ms_checks(self):
        for end in ((100,100),(1600,800),(2500,1300)):
            normal=plan_motion((0,0),end)
            fast=plan_motion((0,0),end,profile='collection_fast')
            self.assertEqual(normal.control_points,fast.control_points)
            self.assertEqual(fast.path_type,'cubic_bezier_ease_in_out')
            self.assertLess(fast.duration_ms,normal.duration_ms)
            self.assertTrue(60<=fast.duration_ms<=120)
            self.assertEqual(fast.points[-1].position,end)
            self.assertGreater(len(fast.points),5)
            self.assertTrue(all(0<b.offset_ms-a.offset_ms<=12.00001 for a,b in zip(fast.points,fast.points[1:])))
            cursor=FakeCursor(start=(0,0));result=cursor.run(fast)
            self.assertTrue(result['completed'])
            self.assertEqual(result['timing_profile'],'collection_fast')
            self.assertEqual(cursor.position,end)

    def test_fast_motion_still_stops_on_interference_before_a_click(self):
        cursor=FakeCursor(start=(0,0))
        def interfere(c):
            if len(c.moves)>=2:c.position=(c.position[0]+20,c.position[1])
        cursor.on_set=interfere
        with self.assertRaises(MotionInterrupted):
            cursor.move_then_fake_click(plan_motion((0,0),(1900,1100),profile='collection_fast'))
        self.assertEqual(cursor.clicks,0)

    def test_fast_profile_requires_explicit_valid_profile_and_bound(self):
        with self.assertRaisesRegex(ValueError,'DURATION'):
            plan_motion((0,0),(100,100),duration_ms=60)
        for value in (True,10,121,float('nan')):
            with self.assertRaises(ValueError):plan_motion((0,0),(100,100),duration_ms=value,profile='collection_fast')
        for profile in (None,True,'unknown'):
            with self.assertRaises(ValueError):plan_motion((0,0),(100,100),profile=profile)
        plan=plan_motion((1,2),(1,2),profile='collection_fast')
        self.assertEqual(plan.duration_ms,0)
        self.assertEqual(plan.timing_profile,'collection_fast')


class RetryPacingTests(unittest.TestCase):
    def test_only_negative_selection_preflight_uses_short_poll(self):
        for negative in (False,True):
            clock=fake.Clock()
            pending=dict(passed=False,exit_status=0,collection_selection_preflight_retryable=negative)
            if not negative:pending.update(exit_status=1,result=dict(page_error='E_DIAGNOSTIC_PAGE_MISMATCH',startup_page=dict(page='unknown')))
            observations=iter([pending,dict(passed=True)])
            result=stabilize_capture(lambda _:next(observations),lambda:True,now=clock.now,wait=clock.wait,
                deadline=clock.now()+5,retry_observation=lambda x:x.get('collection_selection_preflight_retryable') is True,
                retry_delay=lambda x:.035 if x.get('collection_selection_preflight_retryable') is True else .15)
            self.assertEqual(clock.waits,[.035 if negative else .15])
            self.assertEqual(result['attempt_count'],2)
            self.assertTrue(result['passed'])

    def test_bad_delay_never_becomes_busy_loop(self):
        for value in (0,-1,True,float('nan'),.501):
            clock=fake.Clock()
            with self.assertRaises(ValueError):
                stabilize_capture(lambda _:dict(passed=False),lambda:True,now=clock.now,wait=clock.wait,
                    deadline=clock.now()+5,retry_observation=lambda _:True,retry_delay=lambda _:value)
            self.assertEqual(clock.waits,[])


class FastCollectionDispatchTests(unittest.TestCase):
    def test_only_verified_collection_uses_fast_click_and_guard_still_runs(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);snapshot=root/'artifacts/trial/input/snapshot.json';snapshot.parent.mkdir(parents=True)
            snapshot.write_text(json.dumps(fake.snapshot()),'utf8')
            clock=fake.Clock();backend=fake.FakeBackend(clock,[fake.packet(),fake.packet('b',gold=True)])
            backend.fast_collection_motion=True
            calls=[]
            def fast_click(point,before_dispatch=None):
                self.assertIsNotNone(before_dispatch)
                calls.append(list(point));return backend.click(point,before_dispatch)
            backend.fast_collection_click=fast_click
            session=ForegroundSession('artifacts/trial/fast.json',root=root,backend=backend,
                now=clock.now,wait=clock.wait,fast_settle=True)
            with session:
                session.perform(fake.capture())
                session.perform(fake.collect())
                session.perform(fake.capture(expect_collection_added=True))
                session.perform(dict(kind='click',expected_before='skin_listings',point=[168,1403]))
            self.assertEqual(len(calls),1)
            self.assertEqual(session.report['manual_clicks'],2)
            self.assertIsNone(session.pending)
            journal=json.loads(next(session.journal.directory.glob('*.json')).read_text('utf8'))
            self.assertEqual(journal['status'],'confirmed')


if __name__=='__main__':unittest.main()
