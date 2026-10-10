"""Pure fake cursor/clock checks; never read or move the real mouse."""
from dataclasses import replace
import json
import math
import time
import unittest

from cursor_motion import MotionInterrupted, MotionPoint, execute_motion, plan_motion


class FakeCursor:
    def __init__(self, start=(10, 20)):
        self.position = start
        self.time = 1000.0
        self.moves = []
        self.waits = []
        self.active = True
        self.user_clear = True
        self.on_wait = None
        self.on_set = None
        self.set_ok = True
        self.clicks = 0

    def clock(self):
        return self.time

    def get_pos(self):
        return self.position

    def set_pos(self, x, y):
        self.moves.append((self.time, x, y))
        if self.set_ok:
            self.position = x, y
        if self.on_set:
            self.on_set(self)
        return self.set_ok

    def foreground(self):
        return self.active

    def check(self):
        return self.user_clear

    def wait(self, seconds):
        self.waits.append(seconds)
        self.time += seconds
        if self.on_wait:
            self.on_wait(self)

    def run(self, plan, **extra):
        return execute_motion(plan, get_pos=self.get_pos, set_pos=self.set_pos,
            foreground=self.foreground, check=self.check, wait=self.wait, clock=self.clock, **extra)

    def move_then_fake_click(self, plan, **extra):
        result = self.run(plan, **extra)
        self.clicks += 1
        return result


class PlanningTests(unittest.TestCase):
    def test_deterministic_curve_has_actual_endpoints_and_intermediate_points(self):
        a = plan_motion((10, 20), (1510, 620))
        self.assertEqual(a, plan_motion((10, 20), (1510, 620)))
        self.assertEqual(a.points[0].position, (10, 20))
        self.assertEqual(a.points[-1].position, (1510, 620))
        self.assertGreater(len(set(p.position for p in a.points)), 10)
        self.assertEqual(a.path_type, 'cubic_bezier_ease_in_out')
        self.assertTrue(any(abs((p.x - 10) * 600 - (p.y - 20) * 1500) > 1500 for p in a.points[1:-1]))

    def test_duration_distance_bounded_and_monotonic(self):
        durations = [plan_motion((0, 0), (distance, 0)).duration_ms for distance in (1, 20, 100, 500, 1500, 5000)]
        self.assertEqual(durations, sorted(durations))
        self.assertTrue(all(120 <= d <= 280 for d in durations))
        self.assertEqual(durations[-1], 280)

    def test_explicit_duration_and_intervals(self):
        for duration in (120, 180.5, 280):
            plan = plan_motion((0, 0), (2000, 1000), duration_ms=duration)
            offsets = [p.offset_ms for p in plan.points]
            self.assertEqual(offsets, sorted(offsets))
            self.assertEqual(offsets[-1], duration)
            self.assertTrue(all(0 < b - a <= 12.000001 for a, b in zip(offsets, offsets[1:])))

    def test_easing_slows_both_ends_without_random_overshoot(self):
        plan = plan_motion((0, 0), (1000, 0), duration_ms=240)
        deltas = [math.hypot(b.x - a.x, b.y - a.y) for a, b in zip(plan.points, plan.points[1:])]
        self.assertLess(deltas[0], deltas[len(deltas) // 2] / 3)
        self.assertLess(deltas[-1], deltas[len(deltas) // 2] / 3)
        self.assertTrue(all(0 <= p.x <= 1000 for p in plan.points))

    def test_bounds_clamp_controls_not_endpoints(self):
        bounds = (0, 0, 1000, 500)
        for start, end in [((0, 0), (1000, 500)), ((1000, 0), (0, 500)),
                           ((1000, 500), (0, 0)), ((0, 500), (1000, 500))]:
            plan = plan_motion(start, end, bounds)
            self.assertTrue(all(bounds[0] <= p.x <= bounds[2] and bounds[1] <= p.y <= bounds[3] for p in plan.points))
            self.assertEqual(plan.points[0].position, start)
            self.assertEqual(plan.points[-1].position, end)

    def test_negative_desktop_coordinates_and_unbounded_outside_game_start(self):
        plan = plan_motion((-1800, -100), (2300, 900), (-1920, -500, 2559, 1439))
        self.assertEqual(plan.start, (-1800, -100))
        self.assertEqual(plan.end, (2300, 900))
        self.assertEqual(plan_motion((-2000, 500), (2300, 900)).start, (-2000, 500))

    def test_zero_distance_is_noop(self):
        plan = plan_motion((123, 456), (123, 456))
        self.assertEqual(plan.path_type, 'stationary')
        self.assertEqual(plan.duration_ms, 0)
        self.assertEqual(len(plan.points), 1)

    def test_one_pixel_distance_remains_exact(self):
        plan = plan_motion((10, 20), (11, 20))
        self.assertEqual(plan.points[-1].position, (11, 20))
        self.assertGreater(len(plan.points), 2)

    def test_metadata_serializable_and_plan_frozen(self):
        plan = plan_motion((0, 0), (400, 300))
        data = json.loads(json.dumps(plan.to_dict()))
        self.assertEqual(data['point_count'], len(plan.points))
        self.assertEqual(data['points'][-1]['x'], 400)
        with self.assertRaises(AttributeError):
            plan.end = (0, 0)

    def test_invalid_arguments_fail_before_side_effects(self):
        for start, end, bounds, duration in [
                ((True, 0), (1, 2), None, None), ((0.5, 0), (1, 2), None, None),
                ((0, 0), (1, 2, 3), None, None), ((0, 0), (2 ** 31, 2), None, None),
                ((0, 0), (1, 2), (0, 0, -1, 2), None),
                ((0, 0), (1, 2), (0, 0, 0, 1), None),
                ((0, 0), (1, 2), None, True), ((0, 0), (1, 2), None, 119),
                ((0, 0), (1, 2), None, 281), ((0, 0), (1, 2), None, float('inf'))]:
            with self.subTest(start=start, end=end, duration=duration), self.assertRaises(ValueError):
                plan_motion(start, end, bounds, duration)


class ExecutionTests(unittest.TestCase):
    def test_many_real_position_updates_before_endpoint_and_no_click(self):
        fake = FakeCursor()
        plan = plan_motion(fake.position, (1500, 700))
        result = fake.run(plan)
        self.assertTrue(result['completed'])
        self.assertEqual(fake.position, plan.end)
        self.assertGreater(len(fake.moves), 10)
        self.assertNotEqual(fake.moves[0][1:], plan.end)
        self.assertEqual(fake.moves[-1][1:], plan.end)
        self.assertEqual(result['moved_points'], len(fake.moves))
        self.assertEqual(fake.clicks, 0)
        self.assertAlmostEqual(result['elapsed_ms'], plan.duration_ms, places=2)
        self.assertTrue(all(0 < delay <= .012000001 for delay in fake.waits))
        self.assertEqual(json.loads(json.dumps(result))['final_position'], [1500, 700])

    def test_zero_move_still_checks_foreground_and_input(self):
        fake = FakeCursor()
        result = fake.run(plan_motion(fake.position, fake.position))
        self.assertTrue(result['completed'])
        self.assertGreaterEqual(result['checks'], 2)
        self.assertEqual((fake.moves, fake.waits), ([], []))

    def test_actual_start_changed_aborts_before_any_move_or_click(self):
        fake = FakeCursor()
        plan = plan_motion(fake.position, (700, 600))
        fake.position = (11, 20)
        with self.assertRaisesRegex(MotionInterrupted, 'CURSOR_START_CHANGED') as raised:
            fake.move_then_fake_click(plan)
        self.assertEqual((fake.moves, fake.clicks), ([], 0))
        self.assertFalse(raised.exception.metadata['completed'])
        self.assertEqual(raised.exception.metadata['final_position'], [11, 20])

    def test_foreground_absent_at_start_no_motion(self):
        fake = FakeCursor()
        fake.active = False
        with self.assertRaisesRegex(MotionInterrupted, 'FOREGROUND_LOST'):
            fake.move_then_fake_click(plan_motion(fake.position, (700, 600)))
        self.assertEqual((fake.moves, fake.clicks), ([], 0))

    def test_foreground_lost_during_wait_stops_immediately(self):
        fake = FakeCursor()
        def interrupt(cursor):
            if len(cursor.waits) == 4:
                cursor.active = False
        fake.on_wait = interrupt
        with self.assertRaisesRegex(MotionInterrupted, 'FOREGROUND_LOST') as raised:
            fake.move_then_fake_click(plan_motion(fake.position, (1400, 1000)))
        self.assertEqual(len(fake.waits), 4)
        self.assertLessEqual(len(fake.moves), 3)
        self.assertEqual(fake.clicks, 0)
        self.assertEqual(raised.exception.metadata['moved_points'], len(fake.moves))

    def test_foreground_lost_by_position_update_stops_before_next_move(self):
        fake = FakeCursor()
        fake.on_set = lambda cursor: setattr(cursor, 'active', False)
        with self.assertRaisesRegex(MotionInterrupted, 'FOREGROUND_LOST'):
            fake.move_then_fake_click(plan_motion(fake.position, (1400, 1000)))
        self.assertEqual(len(fake.moves), 1)
        self.assertEqual(fake.clicks, 0)

    def test_user_button_or_target_check_failure_stops_midway(self):
        fake = FakeCursor()
        def interrupt(cursor):
            if len(cursor.waits) == 3:
                cursor.user_clear = False
        fake.on_wait = interrupt
        with self.assertRaisesRegex(MotionInterrupted, 'USER_CHECK_FAILED'):
            fake.move_then_fake_click(plan_motion(fake.position, (1400, 1000)))
        self.assertEqual(len(fake.waits), 3)
        self.assertLessEqual(len(fake.moves), 2)
        self.assertEqual(fake.clicks, 0)

    def test_manual_cursor_move_during_wait_stops_without_overwriting_user(self):
        fake = FakeCursor()
        def interrupt(cursor):
            if len(cursor.waits) == 4:
                cursor.position = (42, 999)
        fake.on_wait = interrupt
        with self.assertRaisesRegex(MotionInterrupted, 'CURSOR_INTERFERENCE'):
            fake.move_then_fake_click(plan_motion(fake.position, (1400, 1000)))
        self.assertEqual(fake.position, (42, 999))
        self.assertLessEqual(len(fake.moves), 3)
        self.assertEqual(fake.clicks, 0)

    def test_a_stale_read_right_after_our_set_is_read_again(self):
        # Hotkey runs 20261009-191652/-215649, 20261010-003451: the read right
        # after our SetCursorPos still showed the point set just before.
        class StaleCursor(FakeCursor):
            def __init__(self):
                super().__init__()
                self.stale = 0

            def set_pos(self, x, y):
                before = self.position
                ok = super().set_pos(x, y)
                if len(self.moves) == 3:
                    self.stale, self.shown = 2, before    # the next two reads lag behind
                return ok

            def get_pos(self):
                if self.stale:
                    self.stale -= 1
                    return self.shown
                return self.position
        fake = StaleCursor()
        result = fake.move_then_fake_click(plan_motion(fake.position, (1400, 1000)))
        self.assertTrue(result['completed'])
        self.assertEqual(result['stale_reads'], 1)
        self.assertEqual(result['stale_read_positions'][0]['rereads'], 1 + 1)   # the guard's read and one re-read lag
        self.assertNotIn('stale_resets', result)
        self.assertEqual(len(fake.moves), result['moved_points'])     # never set again
        self.assertEqual(fake.clicks, 1)

    def test_a_user_nudge_onto_the_previous_point_still_stops(self):
        # wf_2f93b298-dd2: during a wait, a 1 px nudge back onto the point set
        # before must stay an interference (no recovery outside the read after a set).
        fake = FakeCursor()
        def nudge(cursor):
            if len(cursor.waits) == 6 and len(cursor.moves) >= 2:
                cursor.position = tuple(cursor.moves[-2][1:])
        fake.on_wait = nudge
        with self.assertRaisesRegex(MotionInterrupted, 'CURSOR_INTERFERENCE'):
            fake.move_then_fake_click(plan_motion(fake.position, (1400, 1000)))
        self.assertEqual(fake.clicks, 0)

    def test_a_set_that_never_takes_stops_after_one_more_set(self):
        fake = FakeCursor()
        def stuck(cursor):
            if len(cursor.moves) >= 3:
                cursor.position = tuple(cursor.moves[1][1:])   # every later set is undone (the point set before)
        fake.on_set = stuck
        # Our own set not taking is no user at the mouse (review wf_993b38d0-6b8): its own code.
        with self.assertRaisesRegex(MotionInterrupted, 'CURSOR_SET_NOT_APPLIED') as raised:
            fake.move_then_fake_click(plan_motion(fake.position, (1400, 1000)))
        self.assertEqual(raised.exception.metadata['stale_resets'], 1)
        self.assertEqual(raised.exception.metadata['stale_read_failures'][-1]['outcome'], 'still_previous')
        self.assertEqual(fake.clicks, 0)

    def test_a_set_that_did_not_take_once_is_set_again(self):
        # Live 20261010-011807: after a click on the star the next set stayed
        # on the star for more than a few re-reads.
        fake = FakeCursor()
        def first_set_lost(cursor):
            if len(cursor.moves) == 3:
                cursor.position = tuple(cursor.moves[1][1:])
        fake.on_set = first_set_lost
        result = fake.move_then_fake_click(plan_motion(fake.position, (1400, 1000)))
        self.assertTrue(result['completed'])
        self.assertEqual((result['stale_reads'], result['stale_resets']), (1, 1))
        self.assertEqual(len(fake.moves), result['moved_points'] + result['stale_resets'])
        self.assertEqual(fake.clicks, 1)

    def test_stale_reads_are_bounded_per_motion_and_a_third_point_stops(self):
        from cursor_motion import MAX_STALE_RECOVERIES

        class AlwaysStale(FakeCursor):
            """Every read right after a set lags one read behind."""
            def __init__(self, third=None):
                super().__init__()
                self.lag, self.third = None, third

            def set_pos(self, x, y):
                before = self.position
                ok = super().set_pos(x, y)
                self.lag = before
                return ok

            def get_pos(self):
                if self.lag is not None:
                    shown, self.lag = self.lag, None
                    if self.third is not None and len(self.moves) == 2:
                        self.lag = (shown[0] + 3, shown[1])   # the re-read shows yet another point
                    return shown
                return self.position
        fake = AlwaysStale()
        with self.assertRaisesRegex(MotionInterrupted, 'CURSOR_SET_NOT_APPLIED') as raised:
            fake.move_then_fake_click(plan_motion(fake.position, (1400, 1000)))
        self.assertEqual(raised.exception.metadata['stale_reads'], MAX_STALE_RECOVERIES)
        self.assertEqual(fake.clicks, 0)
        fake = AlwaysStale(third=True)
        with self.assertRaisesRegex(MotionInterrupted, 'CURSOR_INTERFERENCE') as raised:
            fake.move_then_fake_click(plan_motion(fake.position, (1400, 1000)))
        self.assertEqual(raised.exception.metadata['stale_read_failures'][0]['outcome'], 'interference')
        self.assertEqual(len(fake.moves), raised.exception.metadata['moved_points'])   # never set again
        self.assertEqual(fake.clicks, 0)

    def test_failed_set_position_does_not_retry_or_click(self):
        fake = FakeCursor()
        fake.set_ok = False
        with self.assertRaisesRegex(MotionInterrupted, 'SET_POSITION_FAILED') as raised:
            fake.move_then_fake_click(plan_motion(fake.position, (1400, 1000)))
        self.assertEqual(len(fake.moves), 1)
        self.assertEqual(raised.exception.metadata['attempted_points'], 1)
        self.assertEqual(raised.exception.metadata['moved_points'], 0)
        self.assertEqual(fake.clicks, 0)

    def test_os_clipped_cursor_is_detected_after_update(self):
        fake = FakeCursor()
        fake.on_set = lambda cursor: setattr(cursor, 'position', (0, 0))
        with self.assertRaisesRegex(MotionInterrupted, 'CURSOR_INTERFERENCE'):
            fake.move_then_fake_click(plan_motion(fake.position, (1400, 1000)))
        self.assertEqual(len(fake.moves), 1)
        self.assertEqual(fake.clicks, 0)

    def test_final_target_must_be_exact_even_with_tolerance(self):
        fake = FakeCursor()
        end = (1400, 1000)
        def clip_final(cursor):
            if cursor.position == end:
                cursor.position = (end[0] + 1, end[1])
        fake.on_set = clip_final
        with self.assertRaisesRegex(MotionInterrupted, 'ENDPOINT_NOT_REACHED'):
            fake.move_then_fake_click(plan_motion(fake.position, end), interference_tolerance_px=2)
        self.assertEqual(fake.clicks, 0)

    def test_oversleep_budget_aborts_before_more_input(self):
        fake = FakeCursor()
        fake.on_wait = lambda cursor: setattr(cursor, 'time', cursor.time + 1)
        with self.assertRaisesRegex(MotionInterrupted, 'MOTION_TIMEOUT') as raised:
            fake.move_then_fake_click(plan_motion(fake.position, (1400, 1000)))
        self.assertEqual((fake.moves, fake.clicks), ([], 0))
        self.assertGreater(raised.exception.metadata['elapsed_ms'], raised.exception.metadata['duration_budget_ms'])

    def test_small_sleep_overshoot_does_not_accumulate_per_step(self):
        fake = FakeCursor()
        fake.on_wait = lambda cursor: setattr(cursor, 'time', cursor.time + .002)
        plan = plan_motion(fake.position, (1400, 1000), duration_ms=240)
        result = fake.run(plan)
        self.assertLessEqual(result['elapsed_ms'], 242.001)
        self.assertEqual(fake.position, plan.end)

    def test_backward_clock_stops(self):
        fake = FakeCursor()
        fake.on_wait = lambda cursor: setattr(cursor, 'time', 999.0)
        with self.assertRaisesRegex(MotionInterrupted, 'CLOCK_ORDER'):
            fake.move_then_fake_click(plan_motion(fake.position, (1400, 1000)))
        self.assertEqual((fake.moves, fake.clicks), ([], 0))

    def test_stalled_injected_clock_is_bounded(self):
        fake = FakeCursor()
        waits = []
        with self.assertRaisesRegex(MotionInterrupted, 'CLOCK_STALLED'):
            execute_motion(plan_motion(fake.position, (1400, 1000)), get_pos=fake.get_pos,
                set_pos=fake.set_pos, foreground=fake.foreground, check=fake.check,
                wait=lambda seconds: waits.append(seconds), clock=fake.clock)
        self.assertEqual(fake.moves, [])
        self.assertGreater(len(waits), 1)
        self.assertLessEqual(len(waits), 128)

    def test_windows_15_625_ms_quantized_clock_tolerates_short_waits(self):
        fake = FakeCursor()
        quantum = .015625
        def quantized():
            return math.floor(fake.time / quantum) * quantum
        plan = plan_motion(fake.position, (1400, 1000), duration_ms=237.49)
        result = execute_motion(plan, get_pos=fake.get_pos, set_pos=fake.set_pos,
            foreground=fake.foreground, check=fake.check, wait=fake.wait, clock=quantized)
        self.assertTrue(result['completed'])
        self.assertEqual(fake.position, plan.end)
        self.assertGreater(result['clock_stagnant_waits'], 0)
        self.assertGreater(len(fake.moves), 10)
        self.assertLessEqual(result['elapsed_ms'], plan.duration_ms + quantum * 1000 + .001)

    def test_coarse_clock_tiny_remainder_uses_positive_retry_not_spin(self):
        fake = FakeCursor()
        quantum = .015625
        def quantized():
            return math.floor(fake.time / quantum) * quantum
        plan = plan_motion(fake.position, (1000, 900), duration_ms=125.000001)
        result = execute_motion(plan, get_pos=fake.get_pos, set_pos=fake.set_pos,
            foreground=fake.foreground, check=fake.check, wait=fake.wait, clock=quantized)
        self.assertTrue(result['completed'])
        self.assertLess(len(fake.waits), 200)
        self.assertTrue(any(.001 <= delay <= .012 for delay in fake.waits))

    def test_user_intervention_still_stops_during_repeated_clock_tick(self):
        fake = FakeCursor()
        quantum = .015625
        def quantized():
            return math.floor(fake.time / quantum) * quantum
        fake.on_wait = lambda cursor: setattr(cursor, 'user_clear', False)
        with self.assertRaisesRegex(MotionInterrupted, 'USER_CHECK_FAILED'):
            execute_motion(plan_motion(fake.position, (1400, 1000)), get_pos=fake.get_pos,
                set_pos=fake.set_pos, foreground=fake.foreground, check=fake.check,
                wait=fake.wait, clock=quantized)
        self.assertEqual(len(fake.waits), 1)
        self.assertEqual(fake.moves, [])

    def test_real_short_clock_and_sleep_with_only_in_memory_cursor(self):
        # This checks the current Python/Windows clock implementation without
        # GetCursorPos, SetCursorPos, a desktop binding or any real mouse input.
        fake = FakeCursor((2199, 1212))
        plan = plan_motion(fake.position, (1280, 480), duration_ms=120)
        result = execute_motion(plan, get_pos=fake.get_pos, set_pos=fake.set_pos,
            foreground=fake.foreground, check=fake.check)
        self.assertTrue(result['completed'])
        self.assertEqual(fake.position, plan.end)
        self.assertGreater(len(fake.moves), 5)
        self.assertGreaterEqual(result['wall_elapsed_ms'], 119)
        self.assertLessEqual(result['wall_elapsed_ms'], result['duration_budget_ms'])

    def test_real_quantized_monotonic_with_fake_cursor(self):
        fake = FakeCursor((2176, 1188))
        plan = plan_motion(fake.position, (2199, 1212), duration_ms=125)
        result = execute_motion(plan, get_pos=fake.get_pos, set_pos=fake.set_pos,
            foreground=fake.foreground, check=fake.check, clock=time.monotonic)
        self.assertTrue(result['completed'])
        self.assertEqual(fake.position, plan.end)
        self.assertLessEqual(result['elapsed_ms'], result['duration_budget_ms'])

    def test_independent_wall_watchdog_catches_frozen_clock_oversleep(self):
        fake = FakeCursor()
        def oversleep(seconds):
            time.sleep(.2)
        with self.assertRaisesRegex(MotionInterrupted, 'MOTION_TIMEOUT') as raised:
            execute_motion(plan_motion(fake.position, (1400, 1000), duration_ms=120),
                get_pos=fake.get_pos, set_pos=fake.set_pos, foreground=fake.foreground,
                check=fake.check, wait=oversleep, clock=fake.clock)
        self.assertEqual(fake.moves, [])
        self.assertGreater(raised.exception.metadata['wall_elapsed_ms'], 180)

    def test_callback_failure_has_serializable_metadata(self):
        fake = FakeCursor()
        def broken():
            raise OSError('fake callback failure')
        with self.assertRaisesRegex(MotionInterrupted, 'CALLBACK_FAILED') as raised:
            execute_motion(plan_motion(fake.position, (1400, 1000)), get_pos=fake.get_pos,
                set_pos=fake.set_pos, foreground=broken, wait=fake.wait, clock=fake.clock)
        record = json.loads(json.dumps(raised.exception.metadata))
        self.assertEqual(record['callback_error'], 'fake callback failure')
        self.assertFalse(record['completed'])
        self.assertEqual(fake.moves, [])

    def test_invalid_get_position_or_clock_never_moves(self):
        fake = FakeCursor()
        for get_pos, clock in [(lambda: (0.5, 1), fake.clock), (fake.get_pos, lambda: float('nan'))]:
            with self.assertRaises(MotionInterrupted):
                execute_motion(plan_motion(fake.position, (1400, 1000)), get_pos=get_pos,
                    set_pos=fake.set_pos, foreground=fake.foreground, wait=fake.wait, clock=clock)
        self.assertEqual(fake.moves, [])

    def test_initial_clock_exception_keeps_failure_metadata(self):
        fake = FakeCursor()
        def broken_clock():
            raise OSError('clock callback failed')
        with self.assertRaisesRegex(MotionInterrupted, 'CALLBACK_FAILED') as raised:
            execute_motion(plan_motion(fake.position, (1400, 1000)), get_pos=fake.get_pos,
                set_pos=fake.set_pos, foreground=fake.foreground, wait=fake.wait, clock=broken_clock)
        self.assertEqual(raised.exception.metadata['callback_error'], 'clock callback failed')
        self.assertEqual(fake.moves, [])

    def test_forged_plan_rejected_before_callbacks(self):
        fake = FakeCursor()
        plan = plan_motion(fake.position, (1400, 1000))
        altered = replace(plan, points=(MotionPoint(0, *plan.start), MotionPoint(1, *plan.end)))
        with self.assertRaisesRegex(ValueError, 'PLAN_INVALID'):
            fake.run(altered)
        self.assertEqual((fake.moves, fake.waits), ([], []))


if __name__ == '__main__':
    unittest.main(verbosity=2)
