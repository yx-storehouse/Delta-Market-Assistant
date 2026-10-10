"""Deterministic bounded cursor motion; no Win32, clicking or random behavior.

Coordinates are real screen pixels. Optional bounds are inclusive
(left, top, right, bottom), e.g. the virtual desktop, not necessarily the game.
Planning is pure; execution uses injected cursor, foreground and input checks.
"""
from __future__ import annotations

from dataclasses import dataclass
import math
import time


PATH_TYPE = 'cubic_bezier_ease_in_out'
MIN_DURATION_MS = 120.0
MAX_DURATION_MS = 280.0
TIMING_PROFILES = {'standard': (MIN_DURATION_MS, MAX_DURATION_MS, 10.0),
                   'collection_fast': (60.0, 120.0, 25.0),
                   'collection_continuous': (30.0, 60.0, 50.0)}
MAX_CHECK_INTERVAL_MS = 12.0
# A coarse injected monotonic clock may legitimately repeat for one 15.625 ms
# Windows tick. Retry with a small positive wait, but never spin indefinitely.
MIN_STALLED_RETRY_MS = 1.0
MAX_STALLED_REQUEST_MS = 64.0
MAX_STALLED_WAITS = 128
# A read right after our own SetCursorPos can still return the point set just
# before it (hotkey runs 20261009-191652, -215649, 20261010-003451, all on the
# fast detour: [2112, 327] / [1884, 334]; replaying the guard sequence puts
# every mismatch at the read directly after the set, wf_2f93b298-dd2). Only
# there, and only for exactly that previous point, the position is read again
# (never set again), up to MAX_STALE_READS times STALE_READ_PAUSE_MS apart and
# at most MAX_STALE_RECOVERIES times per motion; it continues only once the
# read shows the point just set. Anything else, and every mismatch at
# a wait, before a set or at the end, stays CURSOR_INTERFERENCE.
# Live 2026-10-10 01:18 (hotkey_runs/20261010-011807): right after a click on
# the star the read stayed on it for more than 3 re-reads 0.5 ms apart; the
# window is about 12 ms now, and a read that is still exactly the previous point
# after it gets the current point set once more (once per motion).
MAX_STALE_READS = 8
STALE_READ_PAUSE_MS = 1.5
MAX_STALE_RECOVERIES = 3
MAX_STALE_RESETS = 1


def _number(value):
    return type(value) in (int, float) and math.isfinite(value)


def _point(value, code='CURSOR_POINT_INVALID'):
    if (not isinstance(value, (tuple, list)) or len(value) != 2
            or any(type(v) is not int or not -(2 ** 31) <= v < 2 ** 31 for v in value)):
        raise ValueError(code)
    return tuple(value)


def _bounds(value):
    if value is None:
        return None
    if (not isinstance(value, (tuple, list)) or len(value) != 4
            or any(type(v) is not int or not -(2 ** 31) <= v < 2 ** 31 for v in value)
            or value[0] > value[2] or value[1] > value[3]):
        raise ValueError('CURSOR_BOUNDS_INVALID')
    return tuple(value)


def _inside(point, bounds):
    return bounds is None or bounds[0] <= point[0] <= bounds[2] and bounds[1] <= point[1] <= bounds[3]


@dataclass(frozen=True)
class MotionPoint:
    offset_ms: float
    x: int
    y: int

    @property
    def position(self):
        return self.x, self.y


@dataclass(frozen=True)
class MotionPlan:
    start: tuple[int, int]
    end: tuple[int, int]
    bounds: tuple[int, int, int, int] | None
    duration_ms: float
    distance_px: float
    control_points: tuple[tuple[float, float], ...]
    points: tuple[MotionPoint, ...]
    path_type: str = PATH_TYPE
    timing_profile: str = 'standard'
    avoid_bounds: tuple[int, int, int, int] | None = None
    route_waypoints: tuple[tuple[int, int], ...] = ()

    def to_dict(self):
        value = dict(path_type=self.path_type, timing_profile=self.timing_profile, coordinate_space='screen_pixels', easing='smoothstep',
            start=list(self.start), end=list(self.end), bounds=list(self.bounds) if self.bounds else None,
            duration_ms=self.duration_ms, distance_px=self.distance_px,
            control_points=[list(p) for p in self.control_points], point_count=len(self.points),
            points=[dict(offset_ms=p.offset_ms, x=p.x, y=p.y) for p in self.points])
        if self.avoid_bounds is not None:
            value.update(avoid_bounds=list(self.avoid_bounds),
                         route_waypoints=[list(p) for p in self.route_waypoints],
                         route_policy='listing_detail_hover_panel_detour_v1',
                         extra_settle_wait_ms=0)
        return value


class MotionInterrupted(RuntimeError):
    def __init__(self, reason, metadata):
        super().__init__(reason)
        self.metadata = dict(metadata, completed=False, error=reason)


def plan_motion(start, end, bounds=None, duration_ms=None, *, profile='standard', avoid_bounds=None):
    """Return an immutable eased cubic path, including actual start and end.

    Standard motion takes 120..280 ms; validated collection inputs can select
    the explicit 60..120 ms collection_fast profile, or the 30..60 ms
    collection_continuous profile. For the same endpoints the latter's default
    planned duration is exactly half of collection_fast. All profiles keep the
    same cubic control points, exact endpoint, per-point checks and maximum
    12 ms check interval; shorter plans use fewer samples of that same curve.
    Existing default/explicit standard and collection_fast plans are unchanged.
    Points have at most a 12 ms planned interval. A zero-distance plan has one
    point and zero duration; tiny distances may repeat rounded pixel positions.
    No random offsets, overshoot, concealment or anti-detection behavior exists.
    """
    start, end, bounds = _point(start), _point(end), _bounds(bounds)
    avoid_bounds = _bounds(avoid_bounds)
    if avoid_bounds is not None and profile != 'collection_continuous':
        raise ValueError('CURSOR_AVOID_PROFILE')
    if not isinstance(profile,str) or profile not in TIMING_PROFILES:
        raise ValueError('CURSOR_PROFILE_INVALID')
    minimum, maximum, distance_divisor = TIMING_PROFILES[profile]
    if not _inside(start, bounds) or not _inside(end, bounds):
        raise ValueError('CURSOR_ENDPOINT_OUT_OF_BOUNDS')
    if duration_ms is not None and (not _number(duration_ms)
            or not minimum <= duration_ms <= maximum):
        raise ValueError('CURSOR_DURATION_INVALID')
    dx, dy = end[0] - start[0], end[1] - start[1]
    distance = math.hypot(dx, dy)
    if distance == 0:
        if avoid_bounds is not None:
            from collection_motion_route import inside
            if inside(start, avoid_bounds):
                raise ValueError('CURSOR_AVOID_ENDPOINT')
        return MotionPlan(start, end, bounds, 0.0, 0.0,
                          (tuple(map(float, start)), tuple(map(float, end))),
                          (MotionPoint(0.0, *start),), 'stationary', profile, avoid_bounds)
    duration = float(duration_ms) if duration_ms is not None else min(maximum, minimum + distance / distance_divisor)
    bend = min(48.0, distance * .08)
    nx, ny = -dy / distance, dx / distance

    def control(fraction):
        x, y = start[0] + dx * fraction + nx * bend, start[1] + dy * fraction + ny * bend
        if bounds is not None:
            x = min(bounds[2], max(bounds[0], x))
            y = min(bounds[3], max(bounds[1], y))
        return x, y

    c1, c2 = control(1 / 3), control(2 / 3)
    count = math.ceil(duration / MAX_CHECK_INTERVAL_MS)
    points = [MotionPoint(0.0, *start)]
    for index in range(1, count):
        t = index / count
        u = t * t * (3 - 2 * t)  # Smoothstep: zero endpoint velocity.
        v = 1 - u
        x = v ** 3 * start[0] + 3 * v * v * u * c1[0] + 3 * v * u * u * c2[0] + u ** 3 * end[0]
        y = v ** 3 * start[1] + 3 * v * v * u * c1[1] + 3 * v * u * u * c2[1] + u ** 3 * end[1]
        position = round(x), round(y)
        if not _inside(position, bounds):
            raise ValueError('CURSOR_PATH_OUT_OF_BOUNDS')
        points.append(MotionPoint(duration * t, *position))
    points.append(MotionPoint(duration, *end))
    if avoid_bounds is not None:
        from collection_motion_route import inside, panel_route, route_samples, segment_intersects_box
        if inside(start, avoid_bounds) or inside(end, avoid_bounds):
            raise ValueError('CURSOR_AVOID_ENDPOINT')
        # A disjoint control hull proves that the original cubic stays out.
        # Otherwise use visibility routing; do not rely on sparse samples to
        # decide whether the unsampled part of a curve may enter the panel.
        hull=(min(start[0],end[0],c1[0],c2[0]),min(start[1],end[1],c1[1],c2[1]),
              max(start[0],end[0],c1[0],c2[0]),max(start[1],end[1],c1[1],c2[1]))
        disjoint=(hull[2]<avoid_bounds[0] or hull[0]>avoid_bounds[2]
                  or hull[3]<avoid_bounds[1] or hull[1]>avoid_bounds[3])
        if not disjoint:
            route=panel_route(start,end,avoid_bounds,bounds)
            points=tuple(MotionPoint(*value) for value in route_samples(route,duration,MAX_CHECK_INTERVAL_MS))
            if (any(not _inside(p.position,bounds) for p in points)
                or any(segment_intersects_box(a.position,b.position,avoid_bounds) for a,b in zip(points,points[1:]))):
                raise ValueError('CURSOR_AVOID_ROUTE_INVALID')
            # Controls are supplied per collinear leg by the route, not the
            # old single-curve controls. Empty signals that distinction.
            return MotionPlan(start,end,bounds,duration,distance,(),points,
                              'piecewise_cubic_ease_in_out',profile,avoid_bounds,route)
    return MotionPlan(start, end, bounds, duration, distance, (c1, c2), tuple(points),
                      timing_profile=profile,avoid_bounds=avoid_bounds)


def execute_motion(plan, *, get_pos, set_pos, foreground, check=None,
                   wait=time.sleep, clock=time.perf_counter,
                   interference_tolerance_px=0, max_overrun_ms=60):
    """Move only; the caller may click only after this function returns success.

    get_pos() -> (x,y); set_pos(x,y) -> bool; foreground() -> bool;
    check() -> bool optionally checks real user buttons, target occlusion, etc.
    Checks run before/after every wait and position update. Unexpected cursor
    changes, foreground loss, active user input or elapsed-budget overrun stop
    immediately at the next check. No restoring, retrying or clicking occurs;
    the one exception is a read directly after our own set that still shows
    the point set just before: it is read again (never set again), see
    MAX_STALE_READS.

    The default high-resolution perf_counter avoids GetTickCount64 granularity
    on Windows/Python versions with a coarse time.monotonic. Repeated injected
    ticks get bounded retries, not a false failure after a single short sleep.
    Absolute deadlines avoid accumulated sleep drift. Injected callbacks should
    be nonblocking; no function can interrupt a callback that itself blocks.
    MotionInterrupted.metadata is suitable for the same log as success output.
    """
    if not isinstance(plan, MotionPlan):
        raise ValueError('CURSOR_PLAN_INVALID')
    # Reject hand-constructed inconsistent plans before invoking OS callbacks.
    expected_plan = plan_motion(plan.start, plan.end, plan.bounds,
                                plan.duration_ms if plan.duration_ms else None,profile=plan.timing_profile,
                                avoid_bounds=plan.avoid_bounds)
    if plan != expected_plan:
        raise ValueError('CURSOR_PLAN_INVALID')
    if not _number(interference_tolerance_px) or not 0 <= interference_tolerance_px <= 5:
        raise ValueError('CURSOR_INTERFERENCE_TOLERANCE')
    if not _number(max_overrun_ms) or not 0 <= max_overrun_ms <= 100:
        raise ValueError('CURSOR_OVERRUN_BUDGET')
    if any(not callable(fn) for fn in (get_pos, set_pos, foreground, wait, clock)) or check is not None and not callable(check):
        raise ValueError('CURSOR_CALLBACK_INVALID')
    metadata = dict(path_type=plan.path_type, timing_profile=plan.timing_profile, coordinate_space='screen_pixels', easing='smoothstep',
        start=list(plan.start), end=list(plan.end), planned_duration_ms=plan.duration_ms,
        duration_budget_ms=plan.duration_ms + max_overrun_ms, distance_px=plan.distance_px,
        planned_points=len(plan.points), attempted_points=0, moved_points=0, checks=0,
        elapsed_ms=0.0, wall_elapsed_ms=0.0, clock_stagnant_waits=0,
        clock_stagnant_requested_ms=0.0, final_position=None, completed=False, error=None,
        stale_reads=0)
    if plan.avoid_bounds is not None:
        metadata.update(avoid_bounds=list(plan.avoid_bounds),route_waypoints=[list(p) for p in plan.route_waypoints],
            route_policy='listing_detail_hover_panel_detour_v1',extra_settle_wait_ms=0)
    # Independent high-resolution wall watchdog still stops an oversleep when
    # the caller's injected scheduling clock is frozen or coarse.
    wall_started = time.perf_counter()
    try:
        started = clock()
    except Exception as error:
        metadata['callback_error'] = str(error) or type(error).__name__
        raise MotionInterrupted('CURSOR_CALLBACK_FAILED', metadata) from error
    if not _number(started):
        raise MotionInterrupted('CURSOR_CLOCK_INVALID', metadata)
    previous_clock = started
    deadline = started + (plan.duration_ms + max_overrun_ms) / 1000

    def stop(code):
        raise MotionInterrupted(code, metadata)

    def now():
        nonlocal previous_clock
        value = clock()
        if not _number(value):
            stop('CURSOR_CLOCK_INVALID')
        if value < previous_clock:
            stop('CURSOR_CLOCK_ORDER')
        previous_clock = value
        metadata['elapsed_ms'] = round((value - started) * 1000, 3)
        wall_elapsed = (time.perf_counter() - wall_started) * 1000
        metadata['wall_elapsed_ms'] = round(wall_elapsed, 3)
        if value > deadline + 1e-9 or wall_elapsed > metadata['duration_budget_ms'] + 1e-6:
            stop('CURSOR_MOTION_TIMEOUT')
        return value

    def read():
        try:
            actual = _point(get_pos(), 'CURSOR_POSITION_INVALID')
        except ValueError:
            stop('CURSOR_POSITION_INVALID')
        metadata['final_position'] = list(actual)
        return actual

    def guard(expected, initial=False, previous=None):
        """previous: only for the read directly after our own set, the point
        set just before it (a stale read of it is re-read, not an interference)."""
        now()
        metadata['checks'] += 1
        if foreground() is not True:
            stop('CURSOR_FOREGROUND_LOST')
        if check is not None and check() is not True:
            stop('CURSOR_USER_CHECK_FAILED')
        actual = read()
        if math.hypot(actual[0] - expected[0], actual[1] - expected[1]) > interference_tolerance_px:
            if initial or previous is None or actual != previous:
                stop('CURSOR_START_CHANGED' if initial else 'CURSOR_INTERFERENCE')
            if metadata['stale_reads'] >= MAX_STALE_RECOVERIES:
                # Exactly on our previous point again: our own sets keep not
                # taking (no user moves the pointer onto that exact pixel).
                metadata.setdefault('stale_read_failures', []).append(
                    dict(read=list(actual), set=list(expected), rereads=0, reset=False, outcome='recovery_limit'))
                stop('CURSOR_SET_NOT_APPLIED')
            failures = metadata.setdefault('stale_read_failures', [])
            reset = False
            for attempt in range(MAX_STALE_READS * 2):
                if attempt == MAX_STALE_READS:
                    # Still exactly on the previous point: the set did not take. Once per motion, set it again.
                    if metadata.get('stale_resets', 0) >= MAX_STALE_RESETS:
                        break
                    metadata['stale_resets'] = metadata.get('stale_resets', 0) + 1
                    reset = True
                    if set_pos(expected[0], expected[1]) is not True:
                        stop('CURSOR_SET_POSITION_FAILED')
                wait(STALE_READ_PAUSE_MS / 1000)
                now()
                actual = read()
                if actual == expected:
                    break
                if actual != previous:
                    failures.append(dict(read=list(actual), set=list(expected), rereads=attempt + 1,
                                         reset=reset, outcome='interference'))
                    stop('CURSOR_INTERFERENCE')
            if actual != expected:
                failures.append(dict(read=list(previous), set=list(expected), rereads=attempt + 1, reset=reset,
                                     outcome='still_previous'))
                stop('CURSOR_SET_NOT_APPLIED')
            if not failures:
                metadata.pop('stale_read_failures')
            metadata['stale_reads'] += 1
            metadata.setdefault('stale_read_positions', []).append(dict(read=list(previous), set=list(expected),
                                                                        rereads=attempt + 1))
        now()

    expected = plan.start
    stalled_waits = 0
    stalled_requested_ms = 0.0
    try:
        guard(expected, initial=True)
        for point in plan.points[1:]:
            target = started + point.offset_ms / 1000
            guard(expected)
            while True:
                before = now()
                remaining = target - before
                if remaining <= 1e-9:
                    break
                # Tiny remainders must not make a quantized clock's next tick
                # unreachable through thousands of near-zero sleeps.
                delay = min(remaining, MAX_CHECK_INTERVAL_MS / 1000)
                if stalled_waits:
                    delay = max(delay, MIN_STALLED_RETRY_MS / 1000)
                wait(delay)
                guard(expected)
                if now() <= before:
                    stalled_waits += 1
                    stalled_requested_ms += delay * 1000
                    metadata['clock_stagnant_waits'] += 1
                    metadata['clock_stagnant_requested_ms'] = round(
                        metadata['clock_stagnant_requested_ms'] + delay * 1000, 6)
                    if stalled_waits >= MAX_STALLED_WAITS or stalled_requested_ms >= MAX_STALLED_REQUEST_MS:
                        stop('CURSOR_CLOCK_STALLED')
                else:
                    stalled_waits = 0
                    stalled_requested_ms = 0.0
            guard(expected)
            if point.position != expected:
                metadata['attempted_points'] += 1
                if set_pos(point.x, point.y) is not True:
                    stop('CURSOR_SET_POSITION_FAILED')
                metadata['moved_points'] += 1
                previous, expected = expected, point.position
                guard(expected, previous=previous)
        guard(plan.end)
        # The tolerance is for interference monitoring, never permission for an
        # approximate final target. A click needs the exact planned endpoint.
        if tuple(metadata['final_position']) != plan.end:
            stop('CURSOR_ENDPOINT_NOT_REACHED')
        metadata['completed'] = True
        return metadata
    except MotionInterrupted:
        raise
    except Exception as error:
        metadata['callback_error'] = str(error) or type(error).__name__
        raise MotionInterrupted('CURSOR_CALLBACK_FAILED', metadata) from error
