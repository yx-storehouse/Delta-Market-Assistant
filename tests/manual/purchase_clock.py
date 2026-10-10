"""Public-notice countdown clock ("对表"); read-only.

The native watch (``--purchase-countdown-watch``) examines every new frame of
the countdown line above the buy button. When the line's pixels change it
reports the presentation time of that frame and of the frame examined just
before it (DXGI LastPresentTime on the QPC clock, floored to ms) and reads the
line. A second boundary of the display therefore lies in (previous, current+1).

If the line then shows s seconds, the display reaches zero exactly s seconds
after that boundary. Every tick gives such an interval; the intersection over
agreeing ticks is the estimate, so each further second narrows it ("每一跳都
校准"). When later watches disagree with earlier ones (the display moved
against the capture clock) the newest calibration wins. A watch near the end
observes the unlock itself and measures the offset between "display reaches
zero" and the remaining-time line appearing.

Times are on the capture clock (QPC ms, the same clock as ``qpc_ms()``); the
watch's clock pairs map them to system time for display only. The system time
is read, never set. Nothing here sends input.
"""
import ctypes
import math
import re
import socket
import statistics
import struct
import time

from purchase_observation import countdown_seconds, normalize, strip_line_icon

SECOND_MS = 1000
# Change events closer than this belong to one boundary (a redraw spread
# over two frames, or the line read while it was being redrawn).
MERGE_MS = 250
# Two estimates agree when within this; presentation jitter is about one
# frame on each side.
AGREE_MS = 60
WATCH_SCHEMA = 'purchase-countdown-watch-v1'


def qpc_ms():
    """The capture clock in this process: QueryPerformanceCounter in ms.
    (time.perf_counter is QPC on Windows; time.monotonic is GetTickCount64
    with 15.6 ms steps and must not be used for this.)"""
    return time.perf_counter() * 1000.0


def system_unix_ms():
    """System time with sub-millisecond resolution (GetSystemTimePreciseAsFileTime)."""
    filetime = ctypes.c_uint64()
    ctypes.windll.kernel32.GetSystemTimePreciseAsFileTime(ctypes.byref(filetime))
    return (filetime.value - 116444736000000000) / 10000.0


def line_state(text):
    """('countdown', seconds) | ('unlocked', None) | ('unread', None).

    The line's own grammar only. A clock icon left of the text may read as at
    most two non-digit characters before it; nothing else is repaired. The
    remaining-time line may lose its 3 px colon in the line read."""
    if not isinstance(text, str) or not text or len(text) > 80:
        return 'unread', None
    value = normalize(text)
    if re.fullmatch(r'\D{0,2}剩余[:：]?\S+', value):
        return 'unlocked', None
    m = re.fullmatch(r'(\D{0,2}?)((?:\d{1,2}小时)?(?:\d{1,2}分)?\d{1,2}秒后解锁购买)', value)
    if not m:
        return 'unread', None
    try:
        return 'countdown', countdown_seconds(m.group(2))
    except ValueError:
        return 'unread', None


def event_text(event, layout='footer'):
    """The line's text from its word boxes without a read clock icon (see
    purchase_observation.strip_line_icon); the joined text if no boxes."""
    words = event.get('words')
    if isinstance(words, list) and words and all(
            isinstance(w, dict) and isinstance(w.get('text'), str)
            and all(type(w.get(k)) in (int, float) and math.isfinite(w[k]) for k in ('x', 'y', 'width', 'height'))
            for w in words):
        kept, _ = strip_line_icon(words, layout)
        return ''.join(w['text'] for w in kept)
    return event.get('text')


def _number(value, name):
    if type(value) not in (int, float) or not math.isfinite(value):
        raise ValueError('PURCHASE_CLOCK_' + name)
    return float(value)


def ticks_from_watch(watch):
    """(baseline, ticks): the line at the first frame, then one entry per
    observed second boundary.

    A change event that reads the same value is no boundary by itself, but it
    may be the first frame of a redraw spread over two frames: the next value
    change within MERGE_MS is then timed from before it. Only an unreadable
    event merges with its neighbour; two readable different values are always
    two boundaries (the unlock may come a fraction of a second after the
    last second)."""
    if not isinstance(watch, dict) or watch.get('schema') != WATCH_SCHEMA:
        raise ValueError('PURCHASE_CLOCK_WATCH_SCHEMA')
    if watch.get('actions_enabled') is not False or watch.get('system_time_changed') is not False:
        raise ValueError('PURCHASE_CLOCK_WATCH_SCOPE')
    events = watch.get('events')
    if not isinstance(events, list) or not events or events[0].get('kind') != 'baseline':
        raise ValueError('PURCHASE_CLOCK_WATCH_BASELINE')
    # A floored present time t means a present in [t, t+1).
    rounding = 1.0 if watch.get('time_rounding') == 'floor_ms' else 0.0
    layout = watch.get('area', 'footer')
    if layout not in ('footer', 'dialog'):
        raise ValueError('PURCHASE_CLOCK_WATCH_AREA')
    first = events[0]
    state, seconds = line_state(event_text(first, layout))
    baseline = dict(state=state, seconds=seconds, source_mono_ms=_number(first.get('source_mono_ms'), 'SOURCE'),
                    text=first.get('text'))
    ticks = []
    previous = baseline['source_mono_ms']
    current = (state, seconds) if state != 'unread' else None
    pending = None
    for event in events[1:]:
        if event.get('kind') != 'change':
            raise ValueError('PURCHASE_CLOCK_WATCH_EVENT')
        after = _number(event.get('source_mono_ms'), 'SOURCE')
        before = _number(event.get('previous_source_mono_ms'), 'SOURCE')
        if not previous <= before < after:
            raise ValueError('PURCHASE_CLOCK_WATCH_ORDER')
        previous = after
        state, seconds = ('unread', None) if event.get('read_skipped') else line_state(event_text(event, layout))
        if state != 'unread' and (state, seconds) == current:
            pending = dict(before=before, after=after)
            if ticks and ticks[-1]['state'] == 'unread':
                ticks[-1]['closed'] = True  # the unreadable change was no boundary
            continue
        start = before
        if pending is not None and state != 'unread' and after - pending['after'] <= MERGE_MS:
            start = pending['before']
        pending = None
        last = ticks[-1] if ticks else None
        if (last is not None and not last.get('closed') and after - last['after_mono_ms'] <= MERGE_MS
                and (last['state'] == 'unread' or state == 'unread')):
            last['events'].append(event.get('index'))
            if state != 'unread':
                last.update(state=state, seconds=seconds, text=event.get('text'))
                current = (state, seconds)
            continue
        if state != 'unread':
            current = (state, seconds)
        ticks.append(dict(before_mono_ms=start, after_mono_ms=after, upper_mono_ms=after + rounding,
                          state=state, seconds=seconds, text=event.get('text'), events=[event.get('index')]))
    for tick in ticks:
        tick.pop('closed', None)
    return baseline, ticks


def clock_offset(watch):
    """System time minus capture clock (ms) at the watch's start and end."""
    pairs = watch.get('clock_pairs')
    if not isinstance(pairs, list) or len(pairs) != 2:
        raise ValueError('PURCHASE_CLOCK_PAIRS')
    offsets = [_number(p.get('unix_ms'), 'PAIR') - _number(p.get('qpc_ms'), 'PAIR') for p in pairs]
    return dict(start_ms=offsets[0], end_ms=offsets[1], change_ms=offsets[1] - offsets[0])


def local_time(unix_ms):
    seconds, millis = divmod(int(round(unix_ms)), 1000)
    return time.strftime('%H:%M:%S', time.localtime(seconds)) + '.%03d' % millis


def _zero_interval(tick):
    shift = tick['seconds'] * SECOND_MS
    return tick['before_mono_ms'] + shift, tick['upper_mono_ms'] + shift


def _agreeing(ticks):
    """Indices of the largest group of ticks whose zero estimates agree; a
    misread value moves its estimate by whole seconds."""
    mids = [sum(_zero_interval(t)) / 2 for t in ticks]
    groups = [[i for i, other in enumerate(mids) if abs(other - mid) <= AGREE_MS] for mid in mids]
    return max(groups, key=len) if groups else []


def _combine(ticks):
    """Intersection of the ticks' zero intervals; when presentation jitter
    leaves it empty, the median with the gap added to the uncertainty."""
    intervals = [_zero_interval(t) for t in ticks]
    lower = max(a for a, _ in intervals)
    upper = min(b for _, b in intervals)
    if lower < upper:
        return dict(estimate_mono_ms=(lower + upper) / 2, uncertainty_ms=(upper - lower) / 2, consistent=True,
                    lower_mono_ms=lower, upper_mono_ms=upper)
    # One interval a frame late among several (review wf_b4f7bdbb-525: about
    # 1.8 % are 1-7 ms late): the others' common part, without it - only when
    # exactly one interval lies wholly after the others' common part (review
    # wf_7aabd0b3-24a: dropping by width could keep the late one and exclude
    # the zero); otherwise the median below.
    if len(intervals) >= 3:
        late = []
        for drop, (a, b) in enumerate(intervals):
            rest = intervals[:drop] + intervals[drop + 1:]
            low, high = max(x for x, _ in rest), min(y for _, y in rest)
            if low < high and a >= high:
                late.append((low, high))
        if len(late) == 1:
            low, high = late[0]
            return dict(estimate_mono_ms=(low + high) / 2, uncertainty_ms=(high - low) / 2, consistent=False,
                        lower_mono_ms=low, upper_mono_ms=high, dropped_intervals=1)
    estimate = statistics.median((a + b) / 2 for a, b in intervals)
    uncertainty = (lower - upper) / 2 + min((b - a) / 2 for a, b in intervals)
    # Bounds for anything built on the estimate: never inverted.
    return dict(estimate_mono_ms=estimate, uncertainty_ms=uncertainty, consistent=False,
                lower_mono_ms=estimate - uncertainty, upper_mono_ms=estimate + uncertainty)


def window_from_read(seconds, source_mono_ms, now_qpc_ms=None):
    """(earliest ms to display zero, latest ms to unlock) from one read of the
    line: showing s seconds at frame time t means display zero in
    (t + (s-1)*1000, t + s*1000 + 1]; the unlock may follow up to a second later."""
    now = qpc_ms() if now_qpc_ms is None else now_qpc_ms
    return source_mono_ms + (seconds - 1) * SECOND_MS - now, source_mono_ms + (seconds + 1) * SECOND_MS + 1 - now


class CountdownClock:
    """Ticks of one listing's countdown across watches, and the estimate."""

    def __init__(self):
        self.watches = []

    def add_watch(self, watch, anchor_seconds=None, window=None):
        """anchor_seconds: the countdown read by full-frame OCR of the watch's
        final frame (its last examined frame). The watch's own estimate must
        lie in the window that this one read allows (live 2026-10-09 clock03
        read the clock icon as a leading digit consistently enough to outvote
        the correct reads). A watch without an anchor stays unverified.
        window: (earliest, latest) display zero allowed from elsewhere; a tick
        placing the zero outside it is a misread and never used (review
        wf_993b38d0-6b8: '0分1秒' read as '0分7秒' planned a watch past the press)."""
        baseline, ticks = ticks_from_watch(watch)
        if window is not None:
            for tick in ticks:
                if tick['state'] == 'countdown':
                    low, high = _zero_interval(tick)
                    if high < window[0] or low > window[1]:
                        tick['outside_window'] = True
        final = ticks[-1] if ticks else baseline
        last = watch.get('last_source_mono_ms')
        anchor_window = None
        if anchor_seconds is not None and type(last) in (int, float) and math.isfinite(last):
            low, high = window_from_read(anchor_seconds, float(last), now_qpc_ms=0.0)
            anchor_window = (low, high - SECOND_MS)  # display zero only, without the unlock allowance
        record = dict(baseline=baseline, ticks=ticks, offset=clock_offset(watch),
                      anchor_seconds=anchor_seconds, anchor_window=anchor_window,
                      final_seconds=final['seconds'], final_state=final['state'],
                      frames=watch.get('frames_examined'), max_gap_ms=watch.get('max_frame_gap_ms'),
                      covered_ms=watch.get('covered_ms'), ended_by=watch.get('ended_by'),
                      first_source_mono_ms=watch.get('first_source_mono_ms'), last_source_mono_ms=last)
        own = self._own(record)
        record['verified'] = None
        if own is not None and anchor_window is not None:
            record['verified'] = own['lower_mono_ms'] < anchor_window[1] and own['upper_mono_ms'] > anchor_window[0]
        record['anchor_mismatch'] = record['verified'] is False
        self.watches.append(record)
        return self.estimate()

    @staticmethod
    def _own(w):
        own = [t for t in w['ticks'] if t['state'] == 'countdown' and not t.get('outside_window')]
        if not own:
            return None
        best = [own[i] for i in _agreeing(own)]
        return dict(_combine(best), ticks=len(own), agreeing_ticks=len(best), members=best)

    def _per_watch(self):
        out = []
        for index, w in enumerate(self.watches):
            if w['anchor_mismatch']:
                continue
            own = self._own(w)
            if own is not None:
                out.append(dict(own, watch=index, verified=w['verified']))
        return out

    def estimate(self):
        """The moment the display reaches zero, on the capture clock."""
        usable = [w for w in self.watches if not w['anchor_mismatch']]
        out = dict(schema='purchase-countdown-clock-v1', watches=len(self.watches),
                   ticks=sum(1 for w in usable for t in w['ticks'] if t['state'] == 'countdown'),
                   anchor_mismatch_watches=[i for i, w in enumerate(self.watches) if w['anchor_mismatch']],
                   display_zero=None, unlock=self.unlock(), system_time_changed=False)
        per_watch = self._per_watch()
        if not per_watch:
            out['reason'] = 'ANCHOR_MISMATCH' if out['anchor_mismatch_watches'] else 'NO_COUNTDOWN_TICK'
            return out
        # The newest verified watch with at least two agreeing ticks is the
        # reference; others join only when they agree with it. An unverified
        # watch never replaces a verified calibration.
        verified = [w for w in per_watch if w['verified']]
        pool = verified or per_watch
        reference = ([w for w in pool if w['agreeing_ticks'] >= 2] or pool)[-1]
        joined = [w for w in per_watch if abs(w['estimate_mono_ms'] - reference['estimate_mono_ms']) <= AGREE_MS]
        candidates = [t for w in joined for t in w['members']]
        best = [candidates[i] for i in _agreeing(candidates)]
        zero = _combine(best)
        all_ticks = [t for w in usable for t in w['ticks'] if t['state'] == 'countdown']
        zero.update(ticks_used=len(best) - zero.get('dropped_intervals', 0), reference_watch=reference['watch'],
                    verified=bool(verified),
                    anchor_mismatch_watches=out['anchor_mismatch_watches'],
                    excluded_watches=[w['watch'] for w in per_watch if w not in joined],
                    outlier_ticks=[dict(seconds=t['seconds'], text=t['text']) for t in all_ticks if t not in best])
        latest = self.watches[-1]['offset']
        zero['estimate_unix_ms'] = zero['estimate_mono_ms'] + latest['end_ms']
        zero['estimate_local_time'] = local_time(zero['estimate_unix_ms'])
        # Phase of the boundaries against the system clock's whole seconds.
        zero['system_second_phase_ms'] = zero['estimate_unix_ms'] % SECOND_MS
        zero['per_watch'] = [{k: v for k, v in w.items() if k != 'members'} for w in per_watch]
        drift_basis = [w for w in per_watch if w['agreeing_ticks'] >= 3]
        if len(drift_basis) > 1:
            # Display against the capture clock between the first and last
            # well-observed watch (live: a whole-second re-sync, clock05->06).
            zero['drift_ms'] = drift_basis[-1]['estimate_mono_ms'] - drift_basis[0]['estimate_mono_ms']
        ordered = sorted(best, key=lambda t: t['after_mono_ms'])
        if len(ordered) >= 2 and ordered[0]['seconds'] != ordered[-1]['seconds']:
            zero['period_ms'] = ((ordered[-1]['after_mono_ms'] - ordered[0]['after_mono_ms'])
                                 / (ordered[0]['seconds'] - ordered[-1]['seconds']))
        out['display_zero'] = zero
        return out

    def unlock(self):
        """The first observed change from a countdown to the unlocked line.

        Frame accurate when the line read a countdown right before it;
        otherwise bracketed from the last frame known to show a countdown
        (across an unreadable change, or the gap between two watches)."""
        last_value = last_countdown_frame = None
        showing = None  # what the line showed after the latest event
        for w in self.watches:
            base = w['baseline']
            if base['state'] == 'unlocked' and last_value is not None and last_countdown_frame is not None:
                return dict(observed=True, coarse=True, before_mono_ms=last_countdown_frame,
                            after_mono_ms=base['source_mono_ms'], last_countdown_seconds=last_value,
                            includes_unread_transition=showing == 'unread')
            showing = base['state']
            if showing == 'countdown':
                last_value, last_countdown_frame = base['seconds'], base['source_mono_ms']
            for tick in w['ticks']:
                if showing == 'countdown':
                    last_countdown_frame = tick['before_mono_ms']
                if tick['state'] == 'unlocked' and last_value is not None:
                    accurate = showing == 'countdown'
                    return dict(observed=True, coarse=not accurate,
                                before_mono_ms=tick['before_mono_ms'] if accurate else last_countdown_frame,
                                after_mono_ms=tick['upper_mono_ms'], last_countdown_seconds=last_value,
                                includes_unread_transition=not accurate)
                showing = tick['state']
                if showing == 'countdown':
                    last_value = tick['seconds']
                elif showing == 'unlocked':
                    last_value = None
            if showing == 'countdown' and type(w['last_source_mono_ms']) in (int, float):
                last_countdown_frame = float(w['last_source_mono_ms'])
        return dict(observed=False)

    def summary(self):
        """Estimate plus, when the unlock was seen, its offset from zero."""
        out = self.estimate()
        zero, unlock = out['display_zero'], out['unlock']
        if zero and unlock['observed'] and unlock['before_mono_ms'] is not None:
            unlock['offset_from_zero_ms'] = dict(lower=unlock['before_mono_ms'] - zero['upper_mono_ms'],
                                                 upper=unlock['after_mono_ms'] - zero['lower_mono_ms'])
            unlock['zero_consistent'] = zero['consistent']
            unlock['zero_verified'] = zero['verified']
            # 0 means the button appears when the display would read 0; about
            # 1000 means the display showed 0分0秒 for a second first.
            unlock['shown_zero_second'] = unlock['last_countdown_seconds'] == 0
        return out

    def zero_window(self, now_qpc_ms=None):
        """(earliest ms to display zero, latest ms to unlock) from now, or
        None. The unlock may come up to one second after display zero until
        a watch has measured it."""
        zero = self.estimate()['display_zero']
        if zero is None:
            return None
        now = qpc_ms() if now_qpc_ms is None else now_qpc_ms
        return zero['lower_mono_ms'] - now, zero['upper_mono_ms'] + SECOND_MS - now

    def remaining_ms(self, now_qpc_ms=None):
        zero = self.estimate()['display_zero']
        if zero is None:
            return None
        return zero['estimate_mono_ms'] - (qpc_ms() if now_qpc_ms is None else now_qpc_ms)


def standard_time(zero, ntp_offset_ms):
    """The zero moment in standard time: system time plus the NTP offset
    (server minus local)."""
    if not zero or ntp_offset_ms is None:
        return None
    unix_ms = zero['estimate_unix_ms'] + ntp_offset_ms
    return dict(estimate_unix_ms=unix_ms, estimate_local_time=local_time(unix_ms),
                second_phase_ms=unix_ms % SECOND_MS, ntp_offset_ms=ntp_offset_ms)


NTP_SERVERS = ('ntp.aliyun.com', 'ntp.tencent.com', 'time.windows.com')
NTP_UNIX_DELTA = 2208988800


def _ntp_time(raw):
    seconds, fraction = struct.unpack('!II', raw)
    return (seconds - NTP_UNIX_DELTA) * 1000.0 + fraction * 1000.0 / 2 ** 32


def sntp_offset(host, timeout=1.5, port=123):
    """One SNTP query: system clock offset to the server (server minus local,
    ms). Read-only: the reply is compared, the system time is never set."""
    # Name lookup and socket setup first: they must not count as network delay.
    address = socket.getaddrinfo(host, port, socket.AF_INET, socket.SOCK_DGRAM)[0][4]
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.settimeout(timeout)
        request = bytearray(48)
        request[0] = 0x23  # leap 0, version 4, client
        local_send = system_unix_ms()
        send_qpc = qpc_ms()
        seconds = local_send / 1000.0 + NTP_UNIX_DELTA
        whole = int(seconds)
        request[40:48] = struct.pack('!II', whole, int((seconds - whole) * 2 ** 32) & 0xFFFFFFFF)
        sock.sendto(bytes(request), address)
        reply, _ = sock.recvfrom(512)
        receive_local = local_send + (qpc_ms() - send_qpc)  # the interval on QPC, not a possibly stepped wall clock
    if len(reply) < 48 or reply[0] & 7 != 4 or not 1 <= reply[1] <= 15 or reply[24:32] != request[40:48]:
        raise ValueError('PURCHASE_NTP_REPLY')
    server_receive, server_send = _ntp_time(reply[32:40]), _ntp_time(reply[40:48])
    offset = ((server_receive - local_send) + (server_send - receive_local)) / 2
    delay = (receive_local - local_send) - (server_send - server_receive)
    return dict(server=host, offset_ms=offset, delay_ms=delay, stratum=reply[1])


def system_clock_check(servers=NTP_SERVERS, query=sntp_offset):
    """Offsets from several servers; the median of the replies that came back."""
    replies, errors = [], []
    for host in servers:
        try:
            replies.append(query(host))
        except (OSError, ValueError) as error:
            errors.append(dict(server=host, error=str(error) or type(error).__name__))
    out = dict(schema='purchase-system-clock-check-v1', replies=replies, errors=errors, system_time_changed=False)
    if replies:
        out['offset_ms'] = statistics.median(r['offset_ms'] for r in replies)
        out['spread_ms'] = max(r['offset_ms'] for r in replies) - min(r['offset_ms'] for r in replies)
    return out
