"""对表 clock model: synthetic frame streams with a known zero moment.

A watch is generated the way the native watch reports it: frames presented
every ~16.7 ms, an event on the first frame whose line differs from the last
read line, timed between the previous examined frame and that frame.
"""
import math
import socket
import struct
import threading
import unittest

from purchase_clock import (CountdownClock, NTP_UNIX_DELTA, clock_offset, line_state, local_time, sntp_offset,
                            standard_time, system_clock_check, ticks_from_watch, window_from_read, WATCH_SCHEMA)
from run_purchase_countdown_probe import (countdown_step_permitted, first_card_selected, listing_identity,
                                          next_watch_ms, only_listing_in_first_slot, same_listing, watch_step)


def text_for(seconds):
    return '%d分%d秒后解锁购买' % (seconds // 60, seconds % 60)


def make_watch(zero, start, duration, *, frame_ms=16.7, phase=3.0, floor=False, unlock_offset=0.0,
               misread=None, read_ms=0.0, offset=1.76e12):
    """The display shows ceil(remaining) (or floor with a shown 0) until
    zero + unlock_offset, then the remaining-time line."""
    def line(t):
        if t >= zero + unlock_offset:
            return '剩余：2天23小时'
        remaining = (zero - t) / 1000.0
        value = math.floor(remaining) if floor else math.ceil(remaining)
        return text_for(max(0, value))
    events, t, previous, reference, busy_until = [], start + phase, None, None, -1.0
    while t < start + duration:
        if t < busy_until:  # frames presented while the line was being read are not examined
            t += frame_ms
            continue
        shown = line(t)
        if reference is None or shown != reference:
            text = shown
            if misread and shown == text_for(misread[0]):
                text = text_for(misread[1])
            events.append(dict(index=len(events), kind='baseline' if reference is None else 'change',
                               source_mono_ms=math.floor(t),
                               previous_source_mono_ms=None if previous is None else math.floor(previous),
                               text=text, ocr_ok=True))
            reference = shown
            busy_until = t + read_ms
        previous = t
        t += frame_ms
    return dict(schema=WATCH_SCHEMA, events=events, frames_examined=int(duration / frame_ms),
                clock_pairs=[dict(qpc_ms=start, unix_ms=start + offset), dict(qpc_ms=start + duration, unix_ms=start + duration + offset)],
                actions_enabled=False, purchase_authorized=False, system_time_changed=False,
                max_frame_gap_ms=frame_ms, first_source_mono_ms=math.floor(start + phase),
                last_source_mono_ms=math.floor(previous), covered_ms=math.floor(previous) - math.floor(start + phase),
                ended_by='time', time_rounding='floor_ms')


def watch_previous_text(events):
    return next(e['text'] for e in reversed(events))


class LineTests(unittest.TestCase):
    def test_countdown_unlocked_and_unread_lines(self):
        self.assertEqual(line_state('0分10秒后解锁购买'), ('countdown', 10))
        self.assertEqual(line_state('28分33秒后解锁购买'), ('countdown', 1713))
        self.assertEqual(line_state('1小时2分3秒后解锁购买'), ('countdown', 3723))
        self.assertEqual(line_state('L0分10秒后解锁购买'), ('countdown', 10))  # the clock icon
        self.assertEqual(line_state('剩余：2天23小时'), ('unlocked', None))
        self.assertEqual(line_state('剩余2天23小时'), ('unlocked', None))       # colon lost in the line read
        self.assertEqual(line_state('60分4秒后解锁购买'), ('unread', None))     # icon + 0分4秒
        for text in ('0分1O秒后解锁购买', '0分10秒后解', '', None, '秒后解锁购买', 'ABC0分10秒后解锁购买'):
            with self.subTest(text=text):
                self.assertEqual(line_state(text), ('unread', None))


class ClockTests(unittest.TestCase):
    def test_one_watch_pins_zero_to_a_frame(self):
        zero = 1_000_000.0 + 87_412.3
        clock = CountdownClock()
        estimate = clock.add_watch(make_watch(zero, zero - 95_000, 10_000))['display_zero']
        self.assertTrue(estimate['consistent'])
        self.assertGreaterEqual(estimate['ticks_used'], 9)
        self.assertLessEqual(abs(estimate['estimate_mono_ms'] - zero), 17)
        self.assertLessEqual(estimate['uncertainty_ms'], 9)
        self.assertLessEqual(estimate['lower_mono_ms'], zero + 17)
        self.assertGreaterEqual(estimate['upper_mono_ms'], zero - 1)
        self.assertAlmostEqual(estimate['period_ms'], 1000, delta=4)

    def test_floored_present_times_never_exclude_the_true_zero(self):
        # Measured stream rate ~140 frames/s; 30 ticks narrow the
        # intersection below one millisecond.
        for phase in (0.1, 0.45, 0.9, 3.3):
            zero = 7_000_000.0 + phase
            clock = CountdownClock()
            clock.add_watch(make_watch(zero, zero - 60_000, 10_000, frame_ms=7.13, phase=phase))
            estimate = clock.add_watch(make_watch(zero, zero - 45_000, 10_000, frame_ms=7.13, phase=phase + 2))['display_zero']
            with self.subTest(phase=phase):
                self.assertTrue(estimate['consistent'])
                self.assertLessEqual(estimate['lower_mono_ms'], zero)
                self.assertGreaterEqual(estimate['upper_mono_ms'], zero)
                self.assertLessEqual(estimate['uncertainty_ms'], 4)

    def test_slow_reads_widen_but_still_contain_the_boundary(self):
        zero = 500_000.0
        estimate = CountdownClock().add_watch(make_watch(zero, zero - 30_000, 9_000, read_ms=70))['display_zero']
        self.assertTrue(estimate['consistent'])
        self.assertLessEqual(abs(estimate['estimate_mono_ms'] - zero), 17)

    def test_each_further_watch_narrows_and_reports_drift(self):
        zero = 2_000_000.0
        clock = CountdownClock()
        first = clock.add_watch(make_watch(zero, zero - 120_000, 4_500, phase=1.0))['display_zero']
        second = clock.add_watch(make_watch(zero, zero - 60_000, 4_500, phase=9.0))['display_zero']
        self.assertLessEqual(second['uncertainty_ms'], first['uncertainty_ms'])
        self.assertEqual(len(second['per_watch']), 2)
        self.assertLessEqual(abs(second['drift_ms']), 20)
        # The display moved +120 ms against the capture clock: the newest
        # calibration wins, the older watch is excluded, the drift reported.
        drifting = CountdownClock()
        drifting.add_watch(make_watch(zero, zero - 120_000, 10_000))
        late = drifting.add_watch(make_watch(zero + 120, zero - 60_000, 4_000))['display_zero']
        self.assertAlmostEqual(late['drift_ms'], 120, delta=20)
        self.assertLessEqual(abs(late['estimate_mono_ms'] - (zero + 120)), 17)
        self.assertEqual((late['reference_watch'], late['excluded_watches']), (1, [0]))

    def test_a_misread_second_is_an_outlier_not_an_estimate(self):
        zero = 3_000_000.0
        estimate = CountdownClock().add_watch(make_watch(zero, zero - 40_000, 8_000, misread=(35, 38)))['display_zero']
        self.assertEqual([o['seconds'] for o in estimate['outlier_ticks']], [38])
        self.assertTrue(estimate['consistent'])
        self.assertLessEqual(abs(estimate['estimate_mono_ms'] - zero), 17)

    def test_one_misread_in_a_short_watch_is_no_drift_and_no_period(self):
        zero = 3_500_000.0
        clock = CountdownClock()
        clock.add_watch(make_watch(zero, zero - 40_000, 10_000))
        estimate = clock.add_watch(make_watch(zero, zero - 6_500, 2_000, misread=(5, 8)))['display_zero']
        self.assertNotIn('drift_ms', estimate)
        self.assertLessEqual(abs(estimate['estimate_mono_ms'] - zero), 17)
        first_misread = CountdownClock().add_watch(make_watch(zero, zero - 39_500, 8_000, misread=(39, 36)))['display_zero']
        self.assertAlmostEqual(first_misread['period_ms'], 1000, delta=4)

    def test_inconsistent_ticks_never_give_inverted_bounds(self):
        zero = 6_000_000.0
        watch = make_watch(zero, zero - 6_000, 9_000)
        late = next(e for e in watch['events'] if e['text'] == text_for(3))
        late['source_mono_ms'] += 25          # one present 25 ms late (vsync)
        late['previous_source_mono_ms'] += 25
        clock = CountdownClock()
        clock.add_watch(watch)
        summary = clock.summary()
        zero_estimate, unlock = summary['display_zero'], summary['unlock']
        self.assertFalse(zero_estimate['consistent'])
        self.assertLess(zero_estimate['lower_mono_ms'], zero_estimate['upper_mono_ms'])
        self.assertFalse(unlock['zero_consistent'])
        self.assertLessEqual(unlock['offset_from_zero_ms']['lower'], 0)
        self.assertGreaterEqual(unlock['offset_from_zero_ms']['upper'], 0)

    def test_unlock_between_two_watches_is_a_coarse_unlock(self):
        zero = 8_000_000.0
        clock = CountdownClock()
        clock.add_watch(make_watch(zero, zero - 8_000, 7_500))
        clock.add_watch(make_watch(zero, zero + 700, 2_000))
        unlock = clock.summary()['unlock']
        self.assertTrue(unlock['observed'] and unlock['coarse'])
        self.assertLessEqual(unlock['before_mono_ms'], zero)
        self.assertGreaterEqual(unlock['after_mono_ms'], zero)

    def test_an_unlock_a_fraction_after_zero_is_its_own_boundary(self):
        # Review: the 剩余 line 150 ms after the 0-second boundary must not be
        # merged into that boundary (MERGE_MS is 250).
        zero = 9_000_000.0
        clock = CountdownClock()
        # Floor display: 0 shown from zero-1000; the unlock 150 ms later.
        clock.add_watch(make_watch(zero, zero - 5_000, 7_000, floor=True, unlock_offset=-850))
        unlock = clock.summary()['unlock']
        self.assertTrue(unlock['observed'] and not unlock['coarse'])
        self.assertEqual(unlock['last_countdown_seconds'], 0)
        offset = unlock['offset_from_zero_ms']
        self.assertLessEqual(offset['lower'], 150)
        self.assertGreaterEqual(offset['upper'], 150)
        self.assertLess(offset['upper'] - offset['lower'], 40)

    def test_a_redraw_whose_first_frame_reads_the_old_value_keeps_the_true_zero(self):
        zero = 9_500_000.0
        watch = make_watch(zero, zero - 9_000, 8_000)
        events = []
        for e in watch['events']:
            if e['kind'] == 'change':
                partial = dict(e, text=watch_previous_text(events))  # first frame: still the old value
                events.append(partial)
                events.append(dict(e, source_mono_ms=e['source_mono_ms'] + 16, previous_source_mono_ms=e['source_mono_ms']))
            else:
                events.append(e)
        for i, e in enumerate(events):
            e['index'] = i
        estimate = CountdownClock().add_watch(dict(watch, events=events))['display_zero']
        self.assertLessEqual(estimate['lower_mono_ms'], zero)
        self.assertGreaterEqual(estimate['upper_mono_ms'], zero)

    def test_unlock_after_an_unreadable_change_is_bracketed_from_the_last_countdown(self):
        zero = 9_800_000.0
        watch = make_watch(zero, zero - 3_000, 6_000)
        for e in watch['events']:
            if e['text'].startswith('剩余'):
                e['text'] = '剩'   # unreadable first unlocked read
                break
        later = make_watch(zero, zero + 3_500, 1_000)
        clock = CountdownClock()
        clock.add_watch(watch)
        clock.add_watch(later)
        unlock = clock.unlock()
        self.assertTrue(unlock['observed'] and unlock['coarse'] and unlock['includes_unread_transition'])
        self.assertLessEqual(unlock['before_mono_ms'], zero)
        self.assertGreaterEqual(unlock['after_mono_ms'], zero)

    def test_unlock_at_zero_or_after_a_shown_zero(self):
        zero = 4_000_000.0
        ceil = CountdownClock()
        ceil.add_watch(make_watch(zero, zero - 6_000, 9_000))
        unlock = ceil.summary()['unlock']
        self.assertTrue(unlock['observed'])
        self.assertEqual(unlock['last_countdown_seconds'], 1)
        self.assertFalse(unlock['shown_zero_second'])
        self.assertLessEqual(unlock['offset_from_zero_ms']['lower'], 0)
        self.assertGreaterEqual(unlock['offset_from_zero_ms']['upper'], 0)
        self.assertLessEqual(unlock['offset_from_zero_ms']['upper'] - unlock['offset_from_zero_ms']['lower'], 40)
        floor = CountdownClock()
        floor.add_watch(make_watch(zero, zero - 6_000, 9_000, floor=True))
        summary = floor.summary()
        self.assertTrue(summary['unlock']['shown_zero_second'])
        self.assertAlmostEqual(sum(summary['unlock']['offset_from_zero_ms'].values()) / 2, 1000, delta=20)
        # A floor display reaches zero one second later than a ceil display
        # of the same unlock: the estimate is "display reaches 0", by design.
        self.assertAlmostEqual(summary['display_zero']['estimate_mono_ms'], zero - 1000, delta=17)

    def test_wall_time_and_phase_come_from_the_clock_pairs(self):
        zero = 1_000_000.0
        watch = make_watch(zero, zero - 20_000, 3_000, offset=1_760_000_000_250.0)
        estimate = CountdownClock().add_watch(watch)['display_zero']
        self.assertAlmostEqual(estimate['estimate_unix_ms'], zero + 1_760_000_000_250.0, delta=17)
        self.assertAlmostEqual(estimate['system_second_phase_ms'], 250, delta=17)
        self.assertEqual(clock_offset(watch)['change_ms'], 0)

    def test_redraw_over_two_frames_is_one_tick(self):
        watch = make_watch(1_000_000.0, 980_000.0, 2_500)
        change = next(e for e in watch['events'] if e['kind'] == 'change')
        # The first changed frame was caught mid-redraw (unreadable); the
        # next frame reads the new value: one boundary, timed by the first.
        extra = dict(change, index=99, source_mono_ms=change['source_mono_ms'] + 17,
                     previous_source_mono_ms=change['source_mono_ms'])
        watch['events'].insert(watch['events'].index(change) + 1, extra)
        change['text'] = '分秒后'
        for i, e in enumerate(watch['events']):
            e['index'] = i
        _, ticks = ticks_from_watch(watch)
        self.assertEqual(len(ticks), 2)
        self.assertEqual(len(ticks[0]['events']), 2)

    def test_invalid_watch_is_rejected(self):
        good = make_watch(1_000_000.0, 990_000.0, 2_000)
        for change, code in ((dict(actions_enabled=True), 'SCOPE'), (dict(system_time_changed=True), 'SCOPE'),
                             (dict(schema='x'), 'SCHEMA'), (dict(events=[]), 'BASELINE')):
            with self.subTest(code=code), self.assertRaisesRegex(ValueError, code):
                ticks_from_watch(dict(good, **change))
        backwards = dict(good, events=[dict(e) for e in good['events']])
        backwards['events'][1]['previous_source_mono_ms'] = backwards['events'][1]['source_mono_ms'] + 1
        with self.assertRaisesRegex(ValueError, 'ORDER'):
            ticks_from_watch(backwards)

    def test_local_time_carries_a_rounded_second(self):
        import time as clock_time
        seconds = 1_760_000_000
        expected = clock_time.strftime('%H:%M:%S', clock_time.localtime(seconds + 1)) + '.000'
        self.assertEqual(local_time(seconds * 1000 + 999.6), expected)
        self.assertTrue(local_time(seconds * 1000 + 12.4).endswith('.012'))

    def test_one_read_gives_the_planning_window(self):
        earliest, latest = window_from_read(10, 1000.0, now_qpc_ms=1500.0)
        self.assertEqual((earliest, latest), (8500.0, 10501.0))

    def test_no_countdown_means_no_estimate(self):
        clock = CountdownClock()
        estimate = clock.add_watch(make_watch(1_000_000.0, 1_001_000.0, 2_000))
        self.assertIsNone(estimate['display_zero'])
        self.assertEqual(estimate['reason'], 'NO_COUNTDOWN_TICK')
        self.assertIsNone(clock.remaining_ms(0))


class LiveClock03Tests(unittest.TestCase):
    """Recorded live watches (tests/fixtures/purchase_countdown_clock03.json):
    the line OCR read the clock icon as a leading "6" on most events."""

    @classmethod
    def setUpClass(cls):
        import json
        from pathlib import Path
        path = Path(__file__).resolve().parents[1] / 'fixtures' / 'purchase_countdown_clock03.json'
        cls.watches = json.loads(path.read_text('utf-8'))['watches']

    def test_recorded_watches_read_the_true_zero(self):
        # The icon word is removed by its position; zero is ~16:39:11.4 on
        # the PC clock (capture clock 1243034441), not 10 hours later.
        clock = CountdownClock()
        for item in self.watches:
            estimate = clock.add_watch(item['watch'], anchor_seconds=item['anchor_seconds'])['display_zero']
        self.assertEqual(estimate['anchor_mismatch_watches'], [])
        self.assertTrue(estimate['consistent'])
        self.assertEqual(estimate['outlier_ticks'], [])
        self.assertAlmostEqual(estimate['estimate_mono_ms'], 1243034441.5, delta=3)
        self.assertLessEqual(estimate['uncertainty_ms'], 3)
        self.assertAlmostEqual(estimate['period_ms'], 1000, delta=1)
        standard = standard_time(estimate, 620.8)
        self.assertLess(abs(((standard['second_phase_ms'] + 500) % 1000) - 500), 15)

    def test_later_live_runs_with_both_icon_readings_agree(self):
        import json
        from pathlib import Path
        path = Path(__file__).resolve().parents[1] / 'fixtures' / 'purchase_countdown_live.json'
        # clock06 (16:36-16:39) is exactly one second earlier than clock04/05
        # (16:13-16:21): the game re-synced its countdown in between.
        expected = dict(clock04=1243034441.0, clock05=1243034441.0, clock06_follow=1243033441.0)
        for run, items in json.loads(path.read_text('utf-8'))['runs'].items():
            clock = CountdownClock()
            for item in items:
                estimate = clock.add_watch(item['watch'], anchor_seconds=item['anchor_seconds'])['display_zero']
            with self.subTest(run=run):
                self.assertEqual((estimate['anchor_mismatch_watches'], estimate['outlier_ticks']), ([], []))
                self.assertAlmostEqual(estimate['estimate_mono_ms'], expected[run], delta=4)
                self.assertLessEqual(estimate['uncertainty_ms'], 4)

    def test_without_boxes_icon_reads_are_unread_and_the_rest_still_verified(self):
        # No word boxes: the icon cannot be located, but "626分" is no valid
        # minute count, so those reads drop out; the reads without the icon
        # (25分57秒, 25分47秒) still give the true zero, checked by the anchor.
        clock = CountdownClock()
        for item in self.watches:
            watch = dict(item['watch'], events=[{k: v for k, v in e.items() if k != 'words'} for e in item['watch']['events']])
            estimate = clock.add_watch(watch, anchor_seconds=item['anchor_seconds'])['display_zero']
        self.assertEqual([w['verified'] for w in clock.watches], [True, True])
        self.assertAlmostEqual(estimate['estimate_mono_ms'], 1243034441.5, delta=10)
        self.assertEqual(line_state('626分0秒后解锁购买'), ('unread', None))

    def test_a_consistent_misread_with_a_correct_last_tick_fails_the_time_check(self):
        # Review case: every tick 10 h off except the final one, which agrees
        # with the full-frame anchor. The time window of the anchor rejects it.
        zero = 5_000_000.0
        watch = make_watch(zero, zero - 30_000, 8_000)
        for e in watch['events'][:-1]:
            e['text'] = e['text'].replace('0分', '10小时0分', 1)
        clock = CountdownClock()
        estimate = clock.add_watch(watch, anchor_seconds=line_state(watch['events'][-1]['text'])[1])
        self.assertTrue(clock.watches[0]['anchor_mismatch'])
        self.assertIsNone(estimate['display_zero'])
        self.assertEqual(estimate['reason'], 'ANCHOR_MISMATCH')

    def test_screen_reader_drops_the_icon_from_the_footer_countdown(self):
        # Live clock04: the full-frame footer read "6" "21" "分" "12" "秒"...
        from purchase_observation import read_screen
        from test_purchase_readonly import packet
        p = packet()
        region = next(r for r in p['purchase_observation']['regions'] if r['kind'] == 'purchase_footer')
        region['words'] = [dict(text=t, x=x, y=y, width=w, height=h) for t, x, y, w, h in
                           (('6', 2064, 1170, 18, 24), ('21', 2089, 1172, 25, 17), ('分', 2116, 1171, 20, 20),
                            ('12', 2138, 1172, 25, 17), ('秒', 2165, 1171, 19, 20), ('后', 2186, 1172, 19, 18),
                            ('解', 2207, 1171, 19, 20), ('锁', 2228, 1171, 19, 20), ('购', 2249, 1171, 19, 20),
                            ('买', 2270, 1172, 19, 19), ('公', 2138, 1231, 23, 23), ('示', 2163, 1232, 23, 22),
                            ('中', 2189, 1231, 21, 24))]
        screen = read_screen(p)
        self.assertEqual(screen['countdown_seconds'], 21 * 60 + 12)
        self.assertEqual([w['text'] for w in screen['countdown_icon_words_removed']], ['6'])
        self.assertEqual(screen['button_state'], 'publicity')

    def test_icon_word_is_removed_only_at_the_icon_position(self):
        from purchase_observation import strip_line_icon
        icon = dict(text='6', x=2063.7, y=1170.1, width=18.6, height=24.0)
        text = [dict(text=t, x=x, y=1171, width=w, height=18) for t, x, w in
                (('25', 2088.3, 26.0), ('分', 2115.7, 19.5), ('59', 2137.8, 25.4), ('秒', 2164.8, 19.5), ('后', 2185.3, 20.0),
                 ('解', 2206.3, 20.0), ('锁', 2227.8, 19.5), ('购', 2248.3, 20.0), ('买', 2269.8, 19.5))]
        kept, removed = strip_line_icon([icon] + text)
        self.assertEqual((removed['text'], ''.join(w['text'] for w in kept)), ('6', '25分59秒后解锁购买'))
        kept, removed = strip_line_icon(text)                       # icon not read
        self.assertIsNone(removed)
        shifted = [dict(w, x=w['x'] - 40) for w in [icon] + text]  # not this layout
        self.assertIsNone(strip_line_icon(shifted)[1])
        # Live clock05: the icon merged into the first word, "618" = icon + 18.
        merged = [dict(text='618', x=2063.7, y=1170, width=50.6, height=24.8)] + [dict(w) for w in text[1:]]
        merged[1]['x'] = 2115.7
        kept, removed = strip_line_icon(merged)
        self.assertEqual((removed['text'], removed['merged'], ''.join(w['text'] for w in kept)),
                         ('6', True, '18分59秒后解锁购买'))

    def test_a_changed_line_that_reads_the_same_is_no_boundary(self):
        watch = make_watch(1_000_000.0, 990_000.0, 3_000)
        first_change = next(i for i, e in enumerate(watch['events']) if e['kind'] == 'change')
        noise = dict(watch['events'][first_change], source_mono_ms=watch['events'][first_change]['source_mono_ms'] + 400,
                     previous_source_mono_ms=watch['events'][first_change]['source_mono_ms'] + 393)
        watch['events'].insert(first_change + 1, noise)
        for i, e in enumerate(watch['events']):
            e['index'] = i
        _, ticks = ticks_from_watch(watch)
        self.assertEqual([t['seconds'] for t in ticks], [9, 8])
        self.assertNotIn(first_change + 1, [i for t in ticks for i in t['events']])


class NtpTests(unittest.TestCase):
    def test_offset_from_a_local_reply_and_a_bad_reply(self):
        server = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        server.bind(('127.0.0.1', 0))
        port = server.getsockname()[1]
        skew_ms = 1234.0
        mode = dict(bad=False)

        def serve():
            for _ in range(2):
                data, peer = server.recvfrom(512)
                sent = struct.unpack('!II', data[40:48])
                now = sent[0] + sent[1] / 2 ** 32 + skew_ms / 1000.0
                whole = int(now)
                stamp = struct.pack('!II', whole, int((now - whole) * 2 ** 32))
                reply = bytearray(48)
                reply[0], reply[1] = 0x24, 2
                reply[24:32] = b'\0' * 8 if mode['bad'] else data[40:48]
                reply[32:40] = reply[40:48] = stamp
                server.sendto(bytes(reply), peer)
        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        try:
            result = sntp_offset('127.0.0.1', port=port)
            self.assertAlmostEqual(result['offset_ms'], skew_ms, delta=25)
            self.assertEqual(result['stratum'], 2)
            mode['bad'] = True
            with self.assertRaisesRegex(ValueError, 'PURCHASE_NTP_REPLY'):
                sntp_offset('127.0.0.1', port=port)
        finally:
            thread.join(2)
            server.close()
        self.assertGreater(NTP_UNIX_DELTA, 0)

    def test_check_reports_median_and_errors_without_setting_time(self):
        replies = dict(a=dict(server='a', offset_ms=10.0), b=dict(server='b', offset_ms=30.0), c=dict(server='c', offset_ms=12.0))

        def query(host):
            if host == 'd':
                raise OSError('timed out')
            return replies[host]
        out = system_clock_check(('a', 'b', 'c', 'd'), query=query)
        self.assertEqual(out['offset_ms'], 12.0)
        self.assertEqual(out['spread_ms'], 20.0)
        self.assertEqual([e['server'] for e in out['errors']], ['d'])
        self.assertFalse(out['system_time_changed'])


class ProbeTests(unittest.TestCase):
    def test_first_card_is_the_top_left_selected_one(self):
        cards = [dict(bounds=[119, 304, 866, 264]), dict(bounds=[996, 305, 865, 262]), dict(bounds=[120, 580, 863, 261])]
        packet = dict(collection_layout=dict(cards=cards), collection_selected_card=dict(selected=True, bounds=[119, 304, 866, 264]))
        self.assertTrue(first_card_selected(packet))
        packet['collection_selected_card']['bounds'] = [996, 305, 865, 262]
        self.assertFalse(first_card_selected(packet))
        self.assertFalse(first_card_selected(dict(collection_layout=dict(cards=cards))))

    def test_a_single_listing_in_the_first_slot_is_the_first_card(self):
        # Live clock02: one watched AWM, no scrollbar, no measured layout.
        def screen(*boxes):
            return dict(regions=dict(listing_text=[dict(text='x', words=[dict(text='x', x=x, y=y, width=w, height=h)
                                                                        for x, y, w, h in boxes])]))
        single = screen((131, 314, 50, 22), (937, 538, 40, 22), (127, 537, 60, 22))
        no_layout = dict(collection_layout=dict(complete=False, error='E_COLLECTION_SCROLLBAR_TRACK'))
        self.assertTrue(only_listing_in_first_slot(single, no_layout))
        self.assertFalse(only_listing_in_first_slot(screen((131, 314, 50, 22), (1010, 314, 50, 22)), no_layout))
        self.assertFalse(only_listing_in_first_slot(screen((131, 600, 50, 22)), no_layout))
        self.assertFalse(only_listing_in_first_slot(screen(), no_layout))
        self.assertFalse(only_listing_in_first_slot(single, dict(collection_layout=dict(error='E_OTHER'))))

    def test_watch_lengths_span_the_zero_moment(self):
        self.assertEqual(next_watch_ms(None), 10000)
        self.assertEqual(next_watch_ms((60000, 61020)), 10000)
        self.assertEqual(next_watch_ms((12000, 13020)), 7500)   # ends 4.5 s before the earliest zero
        self.assertEqual(next_watch_ms((4000, 5020)), 8020)     # spans zero, a possible shown 0 and 3 s
        self.assertEqual(next_watch_ms((-2000, -980)), 2020)
        self.assertEqual(next_watch_ms((-9000, -7980)), 500)
        # From one read of "0分10秒" right away: the first watch already stops
        # short of the end instead of a blind 10 s.
        self.assertLess(next_watch_ms(window_from_read(10, 0.0, now_qpc_ms=600.0)), 10000)

    def test_listing_identity_tolerates_one_unread_field(self):
        def packet(title, fields, wear, bounds=(119, 304, 866, 264)):
            regions = [dict(kind=k, words=[dict(text=t)] if t else []) for k, t in
                       (('product_title', title), ('card_fields', fields), ('selected_detail_precise', wear),
                        ('selected_detail', 'noise'))]
            return dict(collection_observation=dict(regions=regions), collection_selected_card=dict(bounds=list(bounds)))
        a = listing_identity(packet('AS Val突击步枪-黑银先锋', '成色S230', '0.141880)'))
        self.assertTrue(same_listing(a, listing_identity(packet('AS Val突击步枪-黑银先锋', '', 'sc0.141880'))))
        self.assertTrue(same_listing(a, listing_identity(packet('AS Val突击步枪-黑银先锋', '成色S230', '0.141880)'))))
        self.assertTrue(same_listing(a, listing_identity(packet('', '成色S230', '0.141880)'))))
        self.assertFalse(same_listing(a, listing_identity(packet('', '', '0.141880)'))))
        self.assertFalse(same_listing(a, listing_identity(packet('AS Val突击步枪-黑银先锋', '成色S230', '0.187079)'))))
        self.assertFalse(same_listing(a, listing_identity(packet('AS Val突击步枪-黑银先锋', '成色S230', '0.141880)',
                                                                 bounds=(996, 305, 865, 262)))))
        # A watch read has no measured card layout: title and wear decide.
        unmeasured = listing_identity(packet('AS Val突击步枪-黑银先锋', '', '0.141880)'))
        unmeasured['card'] = None
        self.assertTrue(same_listing(a, unmeasured))

    def test_watch_step_becomes_a_streamed_native_watch(self):
        from pathlib import Path as P
        from types import SimpleNamespace
        from collection_live_session import ForegroundSession
        session = ForegroundSession.__new__(ForegroundSession)
        session.root = P('C:/x')
        session.backend = SimpleNamespace(identities=dict(target_hwnd=1, target_pid=2), return_policy='entry_foreground')
        command = session._command(watch_step(6000))
        self.assertEqual(command[command.index('--purchase-countdown-watch') + 1], '6000')
        for option in ('--purchase-observation', '--reuse-capture-resources', '--collection-ready-stream', '--ocr'):
            self.assertEqual(command.count(option), 1, option)
        self.assertNotIn('--expected-page', command)
        self.assertNotIn('--collection-layout', command)
        for bad in (dict(watch_step(6000), countdown_watch_ms=6000.0), dict(watch_step(6000), expected_page='skin_listings'),
                    dict(watch_step(6000), purchase_observation=False)):
            with self.subTest(bad=bad), self.assertRaisesRegex(Exception, 'PURCHASE_COUNTDOWN_WATCH_STEP'):
                session._command(bad)

    def test_only_reads_and_navigation_are_permitted(self):
        self.assertTrue(countdown_step_permitted(watch_step(10000)))
        self.assertFalse(countdown_step_permitted(watch_step(20000)))
        self.assertFalse(countdown_step_permitted(dict(watch_step(1000), expected_page='skin_listings')))
        self.assertFalse(countdown_step_permitted(dict(watch_step(1000), collection_layout=True)))
        self.assertFalse(countdown_step_permitted(dict(kind='click', point=[2174, 1243], expected_before='watchlist_listings')))
        self.assertFalse(countdown_step_permitted(dict(kind='key', key='escape', expected_before='watchlist_listings')))
        self.assertTrue(countdown_step_permitted(dict(kind='click', point=[2310, 274], viewport=[2560, 1440], expected_before='skin_home')))


if __name__ == '__main__':
    unittest.main()
