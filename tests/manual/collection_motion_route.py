"""Pure pointer-route planning around the listing detail hover panel.

No mouse, capture, timing waits or OCR. The panel is only a route exclusion,
never a source of item coordinates or page identity. All targets stay intact.
"""
import heapq
import math


def segment_intersects_box(start, end, box):
    """Closed line segment / inclusive rectangle intersection (slab test)."""
    low, high = 0., 1.
    for origin, delta, first, last in ((start[0], end[0]-start[0], box[0], box[2]),
                                     (start[1], end[1]-start[1], box[1], box[3])):
        if delta == 0:
            if not first <= origin <= last:
                return False
            continue
        a, b = sorted(((first-origin)/delta, (last-origin)/delta))
        low, high = max(low, a), min(high, b)
        if low > high:
            return False
    return True


def inside(point, box):
    return box[0] <= point[0] <= box[2] and box[1] <= point[1] <= box[3]


def panel_route(start, end, avoid, bounds=None):
    """Shortest visible polyline around one padded rectangle; no old frames.

    Sixteen pixels separate the corner waypoints from the excluded panel.
    Every continuous segment is checked, not just emitted cursor positions.
    """
    if inside(start, avoid) or inside(end, avoid):
        raise ValueError('CURSOR_AVOID_ENDPOINT')
    if not segment_intersects_box(start, end, avoid):
        return (start, end)
    x1, y1, x2, y2 = avoid
    corners = ((x1-16, y1-16), (x2+16, y1-16), (x2+16, y2+16), (x1-16, y2+16))
    nodes = [start, end] + [p for p in corners
        if all(-(2**31)<=v<2**31 for v in p) and (bounds is None or inside(p, bounds))]
    queue = [(0., (0,))]
    best = {0: 0.}
    while queue:
        distance, path = heapq.heappop(queue)
        index = path[-1]
        if distance > best[index]:
            continue
        if index == 1:
            return tuple(nodes[i] for i in path)
        for target, point in enumerate(nodes):
            if target == index or target in path or segment_intersects_box(nodes[index], point, avoid):
                continue
            cost = distance + math.dist(nodes[index], point)
            if cost < best.get(target, math.inf):
                best[target] = cost
                heapq.heappush(queue, (cost, path+(target,)))
    raise ValueError('CURSOR_AVOID_NO_ROUTE')


def route_samples(route, duration_ms, max_interval_ms):
    """Piecewise cubic smoothstep with one shared motion-time budget.

    All waypoints are explicit samples. There is no added per-segment pause or
    minimum duration; checks still occur at most max_interval_ms apart. Cubic
    controls are collinear on each leg, keeping the entire curve outside the
    exclusion proven by panel_route, not only its sampled endpoints.
    """
    lengths = [math.dist(a, b) for a, b in zip(route, route[1:])]
    total = sum(lengths)
    if total <= 0:
        return [(0., *route[0])]
    samples = [(0., *route[0])]
    elapsed = 0.
    for leg, (start, end, distance) in enumerate(zip(route, route[1:], lengths)):
        duration = duration_ms*distance/total
        count = max(1, math.ceil(duration/max_interval_ms))
        for i in range(1, count+1):
            t = i/count
            u = t*t*(3-2*t)
            offset = elapsed+duration*t
            if leg == len(lengths)-1 and i == count:
                offset = duration_ms
            samples.append((offset, round(start[0]+(end[0]-start[0])*u),
                            round(start[1]+(end[1]-start[1])*u)))
        elapsed += duration
    return samples
