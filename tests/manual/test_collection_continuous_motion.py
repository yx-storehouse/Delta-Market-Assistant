"""Pure continuous-profile motion checks; never move/read the real cursor."""
from dataclasses import replace
import importlib.util
import math
from pathlib import Path
import sys
import unittest

from cursor_motion import MAX_CHECK_INTERVAL_MS, MotionInterrupted, plan_motion
from test_cursor_motion import FakeCursor


PROFILE = 'collection_continuous'
ROOT = Path(__file__).resolve().parents[2]


class ContinuousPlanningTests(unittest.TestCase):
    def test_short_medium_long_and_reverse_defaults_are_exactly_half_of_fast(self):
        for start, end in [((10, 20), (11, 20)), ((0, 0), (100, 100)),
                           ((0, 0), (750, 0)), ((10, 20), (1300, 1000)),
                           ((2500, 1400), (100, 100)), ((-1900, -300), (2500, 1400)),
                           ((0, 0), (50000, 10000))]:
            with self.subTest(start=start, end=end):
                fast = plan_motion(start, end, profile='collection_fast')
                continuous = plan_motion(start, end, profile=PROFILE)
                self.assertEqual(continuous.duration_ms, fast.duration_ms / 2)
                self.assertEqual(continuous.control_points, fast.control_points)
                self.assertEqual(continuous.path_type, fast.path_type)
                self.assertEqual(continuous.distance_px, fast.distance_px)
                self.assertTrue(30 <= continuous.duration_ms <= 60)
                self.assertEqual(continuous.points[0].position, start)
                self.assertEqual(continuous.points[-1].position, end)
                self.assertEqual(continuous.points[-1].offset_ms, continuous.duration_ms)
                self.assertTrue(all(0 < b.offset_ms-a.offset_ms <= MAX_CHECK_INTERVAL_MS + 1e-8
                    for a, b in zip(continuous.points, continuous.points[1:])))

    def test_same_bounded_curve_does_not_change_default_or_legacy_profiles(self):
        bounds = (-1920, -500, 2559, 1439)
        for start, end in [((-1920, -500), (2559, 1439)), ((2559, -500), (-1920, 1439)),
                           ((0, 0), (1, 0)), ((2400, 1300), (100, 100))]:
            standard = plan_motion(start, end, bounds)
            old = plan_motion(start, end, bounds, profile='collection_fast')
            new = plan_motion(start, end, bounds, profile=PROFILE)
            distance = math.dist(start, end)
            self.assertEqual(standard.duration_ms, min(280., 120. + distance / 10.))
            self.assertEqual(old.duration_ms, min(120., 60. + distance / 25.))
            self.assertEqual(new.control_points, old.control_points)
            self.assertEqual(new.control_points, standard.control_points)
            self.assertTrue(all(bounds[0] <= p.x <= bounds[2] and bounds[1] <= p.y <= bounds[3]
                                for p in new.points))
            self.assertEqual(standard.timing_profile, 'standard')
            self.assertEqual(old.timing_profile, 'collection_fast')
            self.assertEqual(new.to_dict()['timing_profile'], PROFILE)

    def test_frozen_baseline_legacy_plans_match_every_serialized_field(self):
        source = ROOT / 'artifacts/collection_local_hotpath/baseline/source/tests/manual/cursor_motion.py'
        if not source.exists():
            self.skipTest('local frozen pre-continuous baseline absent')
        name = '_continuous_motion_frozen_baseline'
        spec = importlib.util.spec_from_file_location(name, source)
        baseline = importlib.util.module_from_spec(spec)
        sys.modules[name] = baseline
        self.addCleanup(sys.modules.pop, name, None)
        spec.loader.exec_module(baseline)
        for profile in ('standard', 'collection_fast'):
            for start, end in [((0, 0), (0, 0)), ((0, 0), (1, 1)),
                               ((10, 20), (2000, 1000)), ((-1900, -300), (2500, 1400))]:
                for duration in (None, 180. if profile == 'standard' else 80.):
                    with self.subTest(profile=profile, start=start, duration=duration):
                        actual = plan_motion(start, end, duration_ms=duration, profile=profile)
                        old = baseline.plan_motion(start, end, duration_ms=duration, profile=profile)
                        self.assertEqual(actual.to_dict(), old.to_dict())

    def test_explicit_duration_bounds_and_profile_are_strict(self):
        for duration in (30., 45.5, 60.):
            plan = plan_motion((0, 0), (2000, 1000), duration_ms=duration, profile=PROFILE)
            self.assertEqual(plan.duration_ms, duration)
            self.assertTrue(all(0 < b.offset_ms-a.offset_ms <= 12.000001
                                for a, b in zip(plan.points, plan.points[1:])))
        for duration in (True, 0, 29.999, 60.001, float('nan'), float('inf')):
            with self.subTest(duration=duration), self.assertRaisesRegex(ValueError, 'DURATION'):
                plan_motion((0, 0), (100, 100), duration_ms=duration, profile=PROFILE)
        for profile in ('continuous', 'collection_continuous ', None, True):
            with self.subTest(profile=profile), self.assertRaisesRegex(ValueError, 'PROFILE'):
                plan_motion((0, 0), (100, 100), profile=profile)
        with self.assertRaisesRegex(ValueError, 'DURATION'):
            plan_motion((0, 0), (100, 100), duration_ms=30., profile='collection_fast')

    def test_stationary_profile_remains_zero_duration_with_no_move(self):
        plan = plan_motion((10, 20), (10, 20), profile=PROFILE)
        self.assertEqual(plan.duration_ms, 0)
        self.assertEqual(plan.path_type, 'stationary')
        self.assertEqual(plan.timing_profile, PROFILE)
        cursor = FakeCursor()
        result = cursor.run(plan)
        self.assertEqual((cursor.moves, cursor.waits, cursor.clicks), ([], [], 0))
        self.assertTrue(result['completed'])
        self.assertGreaterEqual(result['checks'], 2)


class ContinuousExecutionTests(unittest.TestCase):
    def plan(self, cursor, end=(2200, 1200)):
        return plan_motion(cursor.position, end, profile=PROFILE)

    def test_execution_retains_endpoint_timing_and_every_twelve_ms_checks(self):
        cursor = FakeCursor()
        plan = self.plan(cursor)
        result = cursor.run(plan)
        self.assertTrue(result['completed'])
        self.assertEqual(result['timing_profile'], PROFILE)
        self.assertEqual(cursor.position, plan.end)
        self.assertGreater(len(cursor.moves), 2)
        self.assertEqual(cursor.clicks, 0)
        self.assertAlmostEqual(result['elapsed_ms'], plan.duration_ms, places=2)
        self.assertTrue(all(0 < value <= .012000001 for value in cursor.waits))
        self.assertGreaterEqual(result['checks'], 2 * len(cursor.moves))

    def test_interference_stops_before_next_position_update_and_no_click(self):
        for stage in ('wait', 'set'):
            cursor = FakeCursor()
            def interfere(value):
                value.position = (42, 999)
            if stage == 'wait':cursor.on_wait = interfere
            else:cursor.on_set = interfere
            with self.subTest(stage=stage), self.assertRaisesRegex(MotionInterrupted, 'INTERFERENCE'):
                cursor.move_then_fake_click(self.plan(cursor))
            self.assertEqual(cursor.position, (42, 999))
            self.assertLessEqual(len(cursor.moves), 1)
            self.assertEqual(cursor.clicks, 0)

    def test_foreground_loss_at_start_wait_or_set_prevents_click(self):
        for stage in ('start', 'wait', 'set'):
            cursor = FakeCursor()
            if stage == 'start':cursor.active = False
            elif stage == 'wait':cursor.on_wait = lambda value:setattr(value, 'active', False)
            else:cursor.on_set = lambda value:setattr(value, 'active', False)
            with self.subTest(stage=stage), self.assertRaisesRegex(MotionInterrupted, 'FOREGROUND_LOST'):
                cursor.move_then_fake_click(self.plan(cursor))
            self.assertLessEqual(len(cursor.moves), 1)
            self.assertEqual(cursor.clicks, 0)

    def test_user_button_or_target_guard_failure_stops_before_click(self):
        cursor = FakeCursor()
        cursor.on_wait = lambda value:setattr(value, 'user_clear', False)
        with self.assertRaisesRegex(MotionInterrupted, 'USER_CHECK_FAILED'):
            cursor.move_then_fake_click(self.plan(cursor))
        self.assertEqual(cursor.moves, [])
        self.assertEqual(cursor.clicks, 0)

    def test_final_position_must_still_be_exact_with_interference_tolerance(self):
        cursor = FakeCursor()
        end = (2200, 1200)
        def clip(value):
            if value.position == end:value.position = (end[0] + 1, end[1])
        cursor.on_set = clip
        with self.assertRaisesRegex(MotionInterrupted, 'ENDPOINT'):
            cursor.move_then_fake_click(self.plan(cursor, end), interference_tolerance_px=2)
        self.assertEqual(cursor.clicks, 0)

    def test_forged_profile_or_duration_plan_is_rejected_before_callbacks(self):
        cursor = FakeCursor()
        # 60 ms is legitimately shared by the two explicit profile ranges;
        # use a shorter continuous plan to test an actually invalid relabel.
        plan = self.plan(cursor, end=(300, 200))
        for forged in (replace(plan, timing_profile='collection_fast'),
                       replace(plan, duration_ms=plan.duration_ms + 1)):
            with self.assertRaises(ValueError):cursor.move_then_fake_click(forged)
            self.assertEqual((cursor.moves, cursor.waits, cursor.clicks), ([], [], 0))


if __name__ == '__main__':
    unittest.main()
