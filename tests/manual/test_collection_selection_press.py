"""Inert input-sequencing tests: no real cursor, windows, sleep, or SendInput."""
import ctypes
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from collection_live_session import WindowsBackend
from test_collection_dispatch_timing import Input


class SelectionPressTests(unittest.TestCase):
    def setup_backend(self, returns=(1,1)):
        b=WindowsBackend.__new__(WindowsBackend)
        b.c,b.Input=ctypes,Input
        events=[]
        state=dict(now=1.,point=(123,456),foreground=True)
        b._position=lambda point,purpose:events.append(('move',point,purpose))
        b.foreground=lambda:state['foreground']
        b._idle_input=lambda:True
        b._cursor=lambda:state['point']
        answers=iter(returns)
        def send(count,inputs,size):
            flags=(inputs[0].data.mi.dwFlags if count==2 else
                ctypes.cast(inputs,ctypes.POINTER(Input)).contents.data.mi.dwFlags)
            events.append(('send',count,flags))
            value=next(answers)
            if isinstance(value,BaseException):raise value
            return value
        b.u=SimpleNamespace(SendInput=send)
        def wait(seconds):
            events.append(('wait',seconds));state['now']+=seconds
        return b,events,state,wait

    def run_click(self,b,state,wait):
        with patch('collection_live_session.time.monotonic',side_effect=lambda:state['now']), \
             patch('collection_live_session.time.sleep',side_effect=wait):
            return b.fast_selection_click([123,456])

    def test_single_down_then_bounded_hold_then_single_up(self):
        b,events,state,wait=self.setup_backend()
        self.assertEqual(self.run_click(b,state,wait),2)
        self.assertEqual(events,[('move',[123,456],'collection_click'),('send',1,2),('wait',.024),('send',1,4)])
        d=b.last_dispatch
        self.assertEqual(d['mouse_down_calls'],1)
        self.assertEqual(d['requested_hold_ms'],24)
        self.assertAlmostEqual(d['observed_hold_ms'],24)
        self.assertEqual(d['release_cleanup_events'],0)
        self.assertEqual(d['returned_events'],2)

    def test_failed_down_never_waits_releases_or_retries(self):
        b,events,state,wait=self.setup_backend((0,))
        self.assertEqual(self.run_click(b,state,wait),0)
        self.assertEqual(events,[('move',[123,456],'collection_click'),('send',1,2)])

    def test_foreground_loss_still_releases_and_stops(self):
        b,events,state,wait=self.setup_backend()
        def loss(seconds):wait(seconds);state['foreground']=False
        with self.assertRaisesRegex(RuntimeError,'FOREGROUND_LOST'):
            self.run_click(b,state,loss)
        self.assertEqual(events[-1],('send',1,4))
        self.assertEqual(b.last_dispatch['mouse_down_calls'],1)

    def test_cursor_interference_still_releases_and_stops(self):
        b,events,state,wait=self.setup_backend()
        def move(seconds):wait(seconds);state['point']=(999,999)
        with self.assertRaisesRegex(RuntimeError,'CURSOR_ENDPOINT'):
            self.run_click(b,state,move)
        self.assertEqual(events[-1],('send',1,4))

    def test_interrupted_hold_still_releases(self):
        b,events,state,wait=self.setup_backend()
        def interrupt(seconds):raise KeyboardInterrupt('interrupted hold')
        with self.assertRaises(KeyboardInterrupt):self.run_click(b,state,interrupt)
        self.assertEqual(events[-1],('send',1,4))
        self.assertIn('interrupted hold',b.last_dispatch['error'])

    def test_failed_release_cleanup_never_converts_to_success(self):
        b,events,state,wait=self.setup_backend((1,0,1))
        self.assertEqual(self.run_click(b,state,wait),1)
        self.assertEqual([e for e in events if e[0]=='send'],[('send',1,2),('send',1,4),('send',1,4)])
        self.assertEqual(b.last_dispatch['release_cleanup_events'],1)

    def test_release_exception_cleanup_preserves_error(self):
        b,events,state,wait=self.setup_backend((1,RuntimeError('release failure'),1))
        with self.assertRaisesRegex(RuntimeError,'release failure'):
            self.run_click(b,state,wait)
        self.assertEqual(b.last_dispatch['returned_events'],1)
        self.assertEqual(b.last_dispatch['release_cleanup_events'],1)

    def test_favorite_and_navigation_keep_original_two_event_dispatch(self):
        for method in ('click','fast_collection_click'):
            b,events,state,wait=self.setup_backend((2,))
            with patch('collection_live_session.time.monotonic',side_effect=lambda:state['now']), \
                 patch('collection_live_session.time.sleep',side_effect=AssertionError('unexpected hold')):
                self.assertEqual(getattr(b,method)([123,456]),2)
            self.assertEqual([e for e in events if e[0]=='send'],[('send',2,2)])
            self.assertNotIn('delivery_policy',b.last_dispatch)

    def test_session_routes_only_verified_selection_to_split_press(self):
        from collection_live_session import ForegroundSession
        from collection_scroll import selection_plan
        from test_collection_dynamic import dynamic_packet,rule
        import test_collection_live_session as fake
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            path=root/'artifacts/trial/input/snapshot.json';path.parent.mkdir(parents=True)
            path.write_text(json.dumps(fake.snapshot()),'utf8')
            clock=fake.Clock()
            backend=fake.FakeBackend(clock,[dynamic_packet('a',selected=False),
                dynamic_packet('b'),dynamic_packet('c',gold=True)])
            backend.fast_collection_motion=True
            backend.reuse_capture=True
            calls=[]
            def selection(point,before_dispatch=None):
                calls.append('selection');return backend.click(point,before_dispatch)
            def favorite(point,before_dispatch=None):
                calls.append('favorite');return backend.click(point,before_dispatch)
            backend.fast_selection_click=selection
            backend.fast_collection_click=favorite
            session=ForegroundSession('artifacts/trial/press.json',root=root,backend=backend,
                now=clock.now,wait=clock.wait,fast_settle=True)
            with session:
                session.perform(fake.capture(collection_layout=True))
                for step in selection_plan(session.previous,rule(),'observed-row')['steps']:
                    session.perform(step)
                session.perform(fake.collect())
                session.perform(fake.capture(collection_layout=True,expect_collection_added=True))
                session.perform(dict(kind='click',expected_before='skin_listings',point=[168,1403]))
            self.assertEqual(calls,['selection','favorite'])
            self.assertEqual(session.report['manual_clicks'],3)
            self.assertIsNone(session.pending)
            self.assertIsNone(session.pending_geometry)


if __name__=='__main__':unittest.main()
