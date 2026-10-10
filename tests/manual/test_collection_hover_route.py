"""Pointer-only detours with fake cursor execution and recorded run07 paths."""
from dataclasses import replace
import json
import math
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from collection_motion_route import inside, panel_route, segment_intersects_box
from collection_live_session import WindowsBackend
from cursor_motion import MotionInterrupted, plan_motion
from test_cursor_motion import FakeCursor

PANEL=(1900,350,2449,1295)
DESKTOP=(0,0,2559,1439)
ROOT=Path(__file__).resolve().parents[2]


class HoverRoutePlanningTests(unittest.TestCase):
    def plan(self,start,end,**options):
        return plan_motion(start,end,DESKTOP,profile='collection_continuous',avoid_bounds=PANEL,**options)

    def assert_clear(self,plan):
        self.assertFalse(any(inside(p.position,PANEL) for p in plan.points))
        self.assertTrue(all(not segment_intersects_box(a.position,b.position,PANEL)
                            for a,b in zip(plan.points,plan.points[1:])))
        self.assertEqual(plan.points[0].position,plan.start)
        self.assertEqual(plan.points[-1].position,plan.end)
        self.assertEqual(plan.points[-1].offset_ms,plan.duration_ms)
        self.assertTrue(all(0<b.offset_ms-a.offset_ms<=12.000001 for a,b in zip(plan.points,plan.points[1:])))

    def test_recorded_neutral_to_star_does_not_sweep_condition_help(self):
        start,end=(2340,1355),(2339,320)
        old=plan_motion(start,end,DESKTOP,profile='collection_continuous')
        new=self.plan(start,end)
        self.assertTrue(any(inside(p.position,PANEL) for p in old.points))
        self.assert_clear(new)
        self.assertEqual(new.duration_ms,old.duration_ms)
        self.assertTrue(any(p[0]>PANEL[2] for p in new.route_waypoints))
        self.assertEqual(new.path_type,'piecewise_cubic_ease_in_out')
        self.assertEqual(new.control_points,())

    def test_recorded_right_card_to_star_avoids_mid_panel_hover(self):
        old=plan_motion((1428,414),(2339,320),DESKTOP,profile='collection_continuous')
        new=self.plan(old.start,old.end)
        self.assertTrue(any(inside(p.position,PANEL) for p in old.points))
        self.assert_clear(new)
        self.assertEqual(new.duration_ms,old.duration_ms)
        self.assertTrue(all(p[1]<PANEL[1] for p in new.route_waypoints[1:]))

    def test_all_list_rows_columns_and_both_directions_keep_budget(self):
        for x in (552,1428):
            for y in (414,689,964,1180):
                for start,end in (((x,y),(2339,320)),((2339,320),(x,y))):
                    with self.subTest(start=start,end=end):
                        new=self.plan(start,end)
                        self.assert_clear(new)
                        self.assertEqual(new.duration_ms,plan_motion(start,end,DESKTOP,profile='collection_continuous').duration_ms)
                        self.assertTrue(all(inside(p.position,DESKTOP) for p in new.points))

    def test_legacy_profile_and_no_option_are_unchanged(self):
        for profile in ('standard','collection_fast','collection_continuous'):
            plan=plan_motion((100,100),(200,200),profile=profile)
            self.assertIsNone(plan.avoid_bounds)
            self.assertEqual(plan.route_waypoints,())
            self.assertNotIn('avoid_bounds',plan.to_dict())
        for profile in ('standard','collection_fast'):
            with self.assertRaisesRegex(ValueError,'AVOID_PROFILE'):
                plan_motion((0,0),(10,10),profile=profile,avoid_bounds=PANEL)

    def test_disjoint_curve_is_preserved_not_turned_into_waypoints(self):
        old=plan_motion((50,60),(500,180),DESKTOP,profile='collection_continuous')
        new=self.plan(old.start,old.end)
        self.assertEqual(new.points,old.points)
        self.assertEqual(new.control_points,old.control_points)
        self.assertEqual(new.route_waypoints,())

    def test_same_point_has_zero_duration_and_no_input(self):
        plan=self.plan((2339,320),(2339,320))
        cursor=FakeCursor(start=plan.start)
        result=cursor.run(plan)
        self.assertTrue(result['completed'])
        self.assertEqual((plan.duration_ms,cursor.moves,cursor.waits),(0,[],[]))

    def test_negative_monitor_origin_translates_route_not_targets(self):
        delta=(-2560,-150)
        start=(2340+delta[0],1355+delta[1]);end=(2339+delta[0],320+delta[1])
        rect=tuple(v+delta[i%2] for i,v in enumerate(PANEL))
        bounds=tuple(v+delta[i%2] for i,v in enumerate(DESKTOP))
        base=self.plan((2340,1355),(2339,320))
        translated=plan_motion(start,end,bounds,profile='collection_continuous',avoid_bounds=rect)
        self.assertEqual(translated.duration_ms,base.duration_ms)
        self.assertEqual(translated.route_waypoints,tuple((x+delta[0],y+delta[1]) for x,y in base.route_waypoints))

    def test_bad_rectangles_endpoints_and_no_room_do_not_generate_route(self):
        for rect in ((1,2,0,4),(1,2,3),(1,2,True,5),('x',2,3,4)):
            with self.assertRaises(ValueError):plan_motion((0,0),(10,10),profile='collection_continuous',avoid_bounds=rect)
        for start,end in (((2000,700),(2339,320)),((2339,320),(1900,350))):
            with self.assertRaisesRegex(ValueError,'AVOID_ENDPOINT'):self.plan(start,end)
        with self.assertRaisesRegex(ValueError,'NO_ROUTE'):
            panel_route((0,5),(10,5),(4,0,6,10),(0,0,10,10))

    def test_closed_intersection_keeps_touching_edge_forbidden(self):
        box=(10,10,20,20)
        for a,b in (((0,10),(10,10)),((15,0),(15,30)),((10,20),(20,20)),((15,15),(15,15))):
            self.assertTrue(segment_intersects_box(a,b,box))
        for a,b in (((0,0),(9,30)),((0,9),(30,9)),((21,0),(21,30))):
            self.assertFalse(segment_intersects_box(a,b,box))


class HoverRouteExecutionTests(unittest.TestCase):
    def plan(self):
        return plan_motion((2340,1355),(2339,320),DESKTOP,profile='collection_continuous',avoid_bounds=PANEL)

    def test_no_added_wait_and_exact_endpoint(self):
        plan=self.plan();cursor=FakeCursor(start=plan.start);result=cursor.run(plan)
        self.assertTrue(result['completed'])
        self.assertEqual(cursor.position,plan.end)
        self.assertAlmostEqual(sum(cursor.waits)*1000,plan.duration_ms,places=5)
        self.assertEqual(result['extra_settle_wait_ms'],0)
        self.assertEqual(cursor.clicks,0)
        self.assertTrue(all(not inside((x,y),PANEL) for _,x,y in cursor.moves))

    def test_user_input_foreground_or_pointer_change_stops_before_click(self):
        for kind in ('pointer','foreground','key'):
            cursor=FakeCursor(start=self.plan().start)
            def interrupt(c):
                if kind=='pointer':c.position=(2000,700)
                elif kind=='foreground':c.active=False
                else:c.user_clear=False
            cursor.on_wait=interrupt
            with self.subTest(kind=kind),self.assertRaises(MotionInterrupted):
                cursor.move_then_fake_click(self.plan())
            self.assertEqual(cursor.clicks,0)

    def test_tampered_waypoint_or_exclusion_rejected_before_os_callbacks(self):
        plan=self.plan()
        for fake in (replace(plan,route_waypoints=(plan.start,plan.end)),
                     replace(plan,avoid_bounds=(100,100,200,200)),
                     replace(plan,points=plan.points[:-1])):
            cursor=FakeCursor(start=plan.start)
            with self.assertRaisesRegex(ValueError,'PLAN_INVALID'):cursor.run(fake)
            self.assertEqual(cursor.moves,[])


class RecordedHoverRouteTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        file=ROOT/'artifacts/collection_local_hotpath/run07/session.json'
        if not file.is_file():raise unittest.SkipTest('recorded local run07 absent')
        cls.steps=json.loads(file.read_text('utf8'))['steps']

    def test_all_seven_slow_receipts_have_old_path_panel_crossing_removed(self):
        slow=0;explicit_condition_tooltips=0
        for index,step in enumerate(self.steps):
            packet=step.get('result',{})
            if packet.get('collection_geometry_scope')!='receipt_only':continue
            slow+=1
            self.assertEqual(packet['collection_layout']['error'],'E_COLLECTION_SCROLLBAR_THUMB')
            motion=self.steps[index-1]['motion']
            start,end=tuple(motion['start']),tuple(motion['end'])
            old=plan_motion(start,end,DESKTOP,profile='collection_continuous')
            new=plan_motion(start,end,DESKTOP,profile='collection_continuous',avoid_bounds=PANEL)
            self.assertTrue(any(inside(p.position,PANEL) for p in old.points))
            self.assertFalse(any(segment_intersects_box(a.position,b.position,PANEL) for a,b in zip(new.points,new.points[1:])))
            self.assertAlmostEqual(new.duration_ms,motion['planned_duration_ms'])
            words=[w for r in packet['collection_current_listing_regions']['observations'] if r['kind']=='modal_guard'
                   for w in r['words'] if 1500<w['x']<1860 and 650<w['y']<735]
            text=''.join(w['text'] for w in words)
            if '参数' in text and ('外观' in text or '表现' in text):explicit_condition_tooltips+=1
        self.assertEqual(slow,7)
        self.assertGreaterEqual(explicit_condition_tooltips,4)


class BackendRouteGateTests(unittest.TestCase):
    def backend(self,profile='collection_continuous',viewport=(2560,1440),origin=(-2560,50)):
        b=object.__new__(WindowsBackend)
        b.c=SimpleNamespace(byref=lambda x:x)
        b.w=SimpleNamespace(POINT=lambda x=0,y=0:SimpleNamespace(x=x,y=y))
        b.identities={'target_hwnd':80};b.fast_collection_motion=True;b.collection_motion_profile=profile
        b.foreground=lambda:True;b._idle_input=lambda:True;b.viewport=lambda:viewport
        def client(hwnd,p):p.x,p.y=origin;return True
        b.u=SimpleNamespace(ClientToScreen=client,WindowFromPoint=lambda p:80,GetAncestor=lambda h,f:80)
        b._cursor=lambda:(origin[0]+2339,origin[1]+320)
        b._move_screen=lambda *args,**kwargs:None
        return b

    def test_only_continuous_collection_in_supported_current_viewport_requests_detour(self):
        for profile in ('collection_continuous','collection_fast','standard'):
            for purpose in ('collection_click','click','scroll','hover'):
                b=self.backend(profile=profile)
                with patch.object(b,'_move_screen') as move:
                    b._position([2339,320],purpose)
                expected=(-660,400,-111,1345) if profile=='collection_continuous' and purpose=='collection_click' else None
                self.assertEqual(move.call_args.kwargs['avoid_bounds'],expected)
        b=self.backend(viewport=(2400,1400))
        with patch.object(b,'_move_screen') as move:b._position([2339,320],'collection_click')
        self.assertIsNone(move.call_args.kwargs['avoid_bounds'])

    def test_listing_wheel_uses_collection_profile_and_detour_only_when_fast(self):
        b=self.backend()
        b._cursor=lambda:(-2560+990,50+751)
        with patch.object(b,'_move_screen') as move:
            b._position([990,751],'collection_scroll')
        self.assertEqual(move.call_args.kwargs['avoid_bounds'],(-660,400,-111,1345))
        backend=object.__new__(WindowsBackend)
        backend.u=SimpleNamespace(GetSystemMetrics=lambda code:{76:0,77:0,78:2560,79:1440}[code])
        backend._cursor=lambda:(2339,320)
        backend.fast_collection_motion=True;backend.collection_motion_profile='collection_continuous'
        backend.motion_history=[]
        for purpose,profile in (('collection_scroll','collection_continuous'),('scroll','standard')):
            with patch('collection_live_session.plan_motion',wraps=__import__('cursor_motion').plan_motion) as planner, \
                    patch('collection_live_session.execute_motion',return_value={'completed':True}):
                backend._move_screen((990,751),80,lambda:True,purpose)
            self.assertEqual(planner.call_args.kwargs['profile'],profile)
        calls=[]
        backend._position=lambda point,purpose='input':calls.append(purpose)
        backend.Input=lambda:SimpleNamespace(data=SimpleNamespace(mi=SimpleNamespace(dwFlags=0,mouseData=0)))
        backend.c=SimpleNamespace(byref=lambda x:x,sizeof=lambda x:1)
        backend.foreground=lambda:True;backend._idle_input=lambda:True
        backend.u=SimpleNamespace(SendInput=lambda *args:1)
        self.assertEqual(backend.fast_collection_scroll([990,751],-480),1)
        self.assertEqual(backend.scroll([990,751],-480),1)
        self.assertEqual(calls,['collection_scroll','scroll'])

    def test_start_left_in_panel_does_not_claim_a_clear_detour(self):
        backend=object.__new__(WindowsBackend)
        backend.u=SimpleNamespace(GetSystemMetrics=lambda code:{76:0,77:0,78:2560,79:1440}[code])
        backend._cursor=lambda:(2000,700)
        backend.fast_collection_motion=True;backend.collection_motion_profile='collection_continuous'
        backend.motion_history=[]
        with patch('collection_live_session.execute_motion',return_value={'completed':True}) as execute:
            backend._move_screen((2339,320),80,lambda:True,'collection_click',avoid_bounds=PANEL)
        self.assertIsNone(execute.call_args.args[0].avoid_bounds)
        self.assertEqual(backend.last_motion['hover_route_decision'],'endpoint_inside_panel_original_checked_motion')

    def test_supported_start_keeps_exclusion_in_actual_executor_plan(self):
        backend=object.__new__(WindowsBackend)
        backend.u=SimpleNamespace(GetSystemMetrics=lambda code:{76:0,77:0,78:2560,79:1440}[code])
        backend._cursor=lambda:(2340,1355)
        backend.fast_collection_motion=True;backend.collection_motion_profile='collection_continuous'
        backend.motion_history=[]
        with patch('collection_live_session.execute_motion',return_value={'completed':True}) as execute:
            backend._move_screen((2339,320),80,lambda:True,'collection_click',avoid_bounds=PANEL)
        self.assertEqual(execute.call_args.args[0].avoid_bounds,PANEL)
        self.assertEqual(backend.last_motion['hover_route_decision'],'current_start_and_target_outside_panel')

if __name__=='__main__':unittest.main()
