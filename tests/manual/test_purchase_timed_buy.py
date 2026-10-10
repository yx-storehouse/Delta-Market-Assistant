"""Timed purchase decisions without input: settings, the dialog gate, the
dialog's zero and press decision, the dialog watch plan, the input budget."""
import copy
import json
import math
import tempfile
import unittest
from pathlib import Path

from purchase_clock import CountdownClock, event_text, line_state
from purchase_observation import read_screen
from run_purchase_timed_buy import (DIALOG_BUY_POINT, DIALOG_BUY_RECT, EARLY_FIRST_WATCH_SLACK_MS, EARLY_FRAME_MAX_AGE_MS,
                                    EARLY_LINE_MAX_AGE_MS, EARLY_MAX_FRAME_GAP_MS,
                                    EARLY_PRESS_MAX_LEAD_MS, EARLY_PRESS_MIN_AHEAD_MS, EARLY_PRESS_MIN_LEAD_MS,
                                    LEAN_MARGIN_MS, LEAN_MIN_MS, PRESS_LATE_LIMIT_MS, InputBudget,
                                    DIALOG_PHASE_PRIOR_MS, DIALOG_WATCH_ROUNDS, DIALOG_ZERO_WINDOW_MS, WATCH_MAX_MS,
                                    ZERO_BOUND_SLACK_MS, dialog_zero, earliest_dialog_zero, early_press_decision,
                                    last_line_seconds, last_line_state, next_dialog_watch,
                                    outcome_of, press_decision, probe_zero_ready,
                                    purchase_dialog_problem, read_delay_settings, settings_from, timed_entry_allowed)
from test_purchase_clock import make_watch
from test_purchase_preentry import COUNTDOWN_5S, dialog_words, footer_words, publicity_screen
from test_purchase_readonly import packet

FIXTURES = Path(__file__).resolve().parents[1] / 'fixtures'


def dialog_watch(zero, start, duration, **kw):
    """The dialog shows 0分0秒 from `zero` and unlocks a second later."""
    watch = make_watch(zero + 1000, start, duration, floor=True, unlock_offset=0.0, **kw)
    watch['area'] = 'dialog'
    return watch


def stop_at_zero(watch, frame_ms=16.7):
    """The native --purchase-countdown-stop-on-zero: the watch ends two
    frames after its line reads 0 seconds right after a positive value.
    Returns (watch, end frame time or None)."""
    previous = None
    for event in watch['events']:
        state, seconds = line_state(event_text(event, 'dialog'))
        if state == 'countdown' and seconds == 0 and previous is not None and previous > 0:
            end = math.floor(event['source_mono_ms'] + 2 * frame_ms)
            stopped = dict(watch, events=[e for e in watch['events'] if e['source_mono_ms'] <= end], ended_by='zero',
                           last_source_mono_ms=end, covered_ms=end - watch['first_source_mono_ms'])
            return stopped, end
        if state == 'countdown':
            previous = seconds
    return watch, None


DIALOG_COUNTING = [('0', 1527, 850, 23, 23), ('0', 1558, 852, 11, 17), ('分', 1571, 851, 20, 20), ('3', 1593, 852, 11, 17),
                   ('秒', 1606, 851, 19, 20), ('后', 1627, 852, 19, 18), ('解', 1648, 851, 19, 20), ('锁', 1669, 851, 19, 20),
                   ('购', 1690, 851, 19, 20), ('买', 1711, 852, 13, 19), ('外观购买', 690, 380, 160, 40)]


class SettingsTests(unittest.TestCase):
    def test_entry_time_and_delay_come_from_the_run_settings(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'config.json'
            path.write_text(json.dumps(dict(run_settings=dict(purchaseDelayMs=830, enterBeforeSeconds=3))), 'utf-8')
            self.assertEqual(read_delay_settings(path), (3, 830))
            path.write_text(json.dumps(dict(run_settings={})), 'utf-8')
            self.assertEqual(read_delay_settings(path), (3, 830))
            for bad in (dict(enterBeforeSeconds=6), dict(enterBeforeSeconds=0), dict(purchaseDelayMs=-1),
                        dict(purchaseDelayMs=1.5), dict(enterBeforeSeconds='3')):
                path.write_text(json.dumps(dict(run_settings=bad)), 'utf-8')
                with self.subTest(bad=bad), self.assertRaisesRegex(ValueError, 'RUN_SETTINGS'):
                    read_delay_settings(path)


class DialogGateTests(unittest.TestCase):
    def test_the_insufficient_balance_dialog_is_never_a_purchase_dialog(self):
        # Review: 充值 lies under DIALOG_BUY_POINT on that dialog (dialog02).
        recorded = json.loads((FIXTURES / 'purchase_insufficient_balance.json').read_text('utf-8'))['packet']
        problem = purchase_dialog_problem(read_screen(copy.deepcopy(recorded)))
        self.assertTrue(problem.startswith('WRONG_DIALOG:'))
        self.assertIn('insufficient_balance', problem)

    def test_a_purchase_dialog_needs_its_countdown_line(self):
        self.assertIsNone(purchase_dialog_problem(read_screen(dialog_words(packet(), DIALOG_COUNTING))))
        unlocked_already = [w for w in DIALOG_COUNTING if w[0] not in ('后', '解', '锁', '购', '买')]
        self.assertTrue(purchase_dialog_problem(read_screen(dialog_words(packet(), unlocked_already))).startswith('DIALOG_COUNTDOWN_LINE'))
        self.assertEqual(purchase_dialog_problem(read_screen(dialog_words(packet(), DIALOG_COUNTING[:-1]))), 'NOT_A_PURCHASE_DIALOG')

    def test_entry_only_before_the_unlock_with_a_proven_change(self):
        p = footer_words(packet(), COUNTDOWN_5S + [('230', 2172, 1233, 46, 19)])
        screen = read_screen(p)
        self.assertIsNone(timed_entry_allowed(screen, p, True))
        self.assertEqual(timed_entry_allowed(screen, p, False), 'BUTTON_CHANGE_UNPROVEN')
        self.assertEqual(timed_entry_allowed(publicity_screen(), p, True), 'BUTTON_STATE:publicity')
        after_unlock = read_screen(footer_words(packet(), [('剩', 2107, 1171, 18, 20), ('余', 2128, 1171, 19, 20),
                                                          ('2天23小时', 2171, 1172, 100, 19), ('230', 2172, 1233, 46, 19)]))
        self.assertEqual(timed_entry_allowed(after_unlock, p, True), 'BUTTON_STATE:price_button')


class DialogZeroTests(unittest.TestCase):
    def test_zero_seen_in_the_dialog_is_calibrated_and_checked_against_the_watchlist(self):
        zero = 5_000_000.0
        footer = zero + 107
        clock = CountdownClock()
        prior_only = dialog_zero(clock, footer - 107, footer, True)
        self.assertFalse(prior_only['calibrated'])
        clock.add_watch(dialog_watch(zero, zero - 3000, 3150))
        estimate = dialog_zero(clock, footer - 107, footer, True)
        self.assertTrue(estimate['observed'] and estimate['calibrated'])
        self.assertLessEqual(abs(estimate['zero_ms'] - zero), 17)
        # A whole-second misread far from the watchlist clock is refused.
        far = CountdownClock()
        far.add_watch(dialog_watch(zero + 2000, zero - 1000, 3150))
        self.assertFalse(dialog_zero(far, footer - 107, footer, True)['calibrated'])

    def test_one_tick_without_the_zero_frame_does_not_calibrate(self):
        zero = 5_000_000.0
        clock = CountdownClock()
        clock.add_watch(dialog_watch(zero, zero - 1500, 900))   # sees only 0分1秒
        estimate = dialog_zero(clock, zero, zero + 107, True)
        self.assertEqual(estimate['ticks_used'], 1)
        self.assertFalse(estimate['calibrated'])

    def test_one_watch_runs_through_the_zero_then_one_up_to_the_press(self):
        # Review wf_b0b8309e-39d: the zero lies inside one watch, never in a gap.
        zero, delay, latest = 10_000.0, 830, 10_300.0
        unseen = dict(zero_ms=zero, observed=False, calibrated=False)
        self.assertEqual(next_dialog_watch(7_000, unseen, latest, delay),
                         (int(latest + delay - LEAN_MARGIN_MS - 7_000), True))
        placed = dict(unseen, calibrated=True)            # two agreeing ticks: never past their press
        self.assertEqual(next_dialog_watch(7_000, placed, latest, delay), (int(zero + delay - LEAN_MARGIN_MS - 7_000), True))
        seen = dict(unseen, observed=True, calibrated=True)
        self.assertEqual(next_dialog_watch(zero + 40, seen, latest, delay), (int(zero + delay - LEAN_MARGIN_MS - zero - 40), False))
        self.assertIsNone(next_dialog_watch(zero + delay - LEAN_MARGIN_MS - LEAN_MIN_MS + 1, seen, latest, delay))
        self.assertEqual(next_dialog_watch(0, unseen, 50_000, delay)[0], WATCH_MAX_MS)

    def test_each_watch_still_counting_raises_the_earliest_zero(self):
        # Review wf_a24b57bc-2f5: s seconds shown at the last frame t means
        # 0分0秒 from t + (s-1) s at the soonest, never past the latest zero.
        zero, start = 1_000_000.0, 1_000_000.0 - 2400
        watch = dialog_watch(zero, start, 500)           # 0分3秒, then 0分2秒 from zero - 2000
        last = watch['last_source_mono_ms']
        self.assertEqual(last_line_seconds(watch), 2)
        self.assertGreaterEqual(earliest_dialog_zero(0.0, watch, zero + 300), last + 1000 - ZERO_BOUND_SLACK_MS)
        # Review wf_3215e495-f2d: every value bounds it up to its last frame (0分3秒 up to zero - 2000).
        self.assertGreater(earliest_dialog_zero(0.0, watch, zero + 300), zero - 100)
        self.assertLessEqual(earliest_dialog_zero(0.0, watch, zero + 300), zero)
        self.assertEqual(earliest_dialog_zero(zero - 10, watch, zero + 300), zero - 10)     # never lowered
        self.assertEqual(earliest_dialog_zero(0.0, watch, last), 0.0)                      # past the latest: a misread
        shown_zero = dialog_watch(zero, zero + 20, 300)                                    # 0分0秒 at the end
        self.assertEqual(earliest_dialog_zero(5.0, shown_zero, zero + 300), 5.0)

    def simulate_dialog_phase(self, enter_s, delay, offset, footer_calibrated=True, gap=5, misread=None,
                              misread_watch=0, overhead=12):
        """dialog_phase's watch loop on a synthetic dialog (first watch about
        1155 ms after the entry click, as live; live watches return 9-19 ms
        after their end). misread: (shown, read) on watch misread_watch.
        Returns (press refusal, calibrated zero error, latest watch end
        relative to the true press)."""
        footer = 1_000_000.0
        real_zero = footer + offset
        window = (footer - 20, footer + 20) if footer_calibrated else (footer - 500, footer + 501)
        earliest, latest = window[0] + DIALOG_ZERO_WINDOW_MS[0], window[1] + DIALOG_ZERO_WINDOW_MS[1]
        prior, clock = footer + DIALOG_PHASE_PRIOR_MS, CountdownClock()
        now, state, ends = footer - 1000 * enter_s + 1155, 'unread', []
        for index in range(DIALOG_WATCH_ROUNDS):
            zero = dialog_zero(clock, prior, footer, footer_calibrated, (earliest, latest))
            planned = next_dialog_watch(now, zero, latest, delay)
            if planned is None:
                break
            ms, stop_on_zero = planned
            watch = dialog_watch(real_zero, now, ms, misread=misread if index == misread_watch else None)
            stopped_at = None
            if stop_on_zero:
                watch, stopped_at = stop_at_zero(watch)
            clock.add_watch(watch, window=(earliest, latest))
            earliest = earliest_dialog_zero(earliest, watch, latest)
            state = last_line_state(watch)
            now = (stopped_at + overhead) if stopped_at is not None else now + ms + overhead
            ends.append(now)
            now += gap
        zero = dialog_zero(clock, prior, footer, footer_calibrated, (earliest, latest))
        click = zero['zero_ms'] + delay
        refused = press_decision(zero, state, now, click, max(now, click) + 3, 1000 * enter_s)
        return refused, zero['zero_ms'] - real_zero, max(ends) - (real_zero + delay)

    def test_short_entries_and_small_delays_still_calibrate_and_press_on_time(self):
        # Reviews wf_a24b57bc-2f5 / wf_b0b8309e-39d: 提前进入 2 s, small delays,
        # live watch overheads and a dialog far ahead of the watchlist. Every
        # zero the dialog shows after its first watch began is seen and pressed.
        for enter_s in (2, 3, 4, 5):
            for delay in (160, 300, 795, 1200):
                for offset in range(-1100, 300, 50):    # inside DIALOG_ZERO_WINDOW_MS
                    for calibrated in (True, False):
                        for gap in (2, 9, 20):
                            refused, error, overrun = self.simulate_dialog_phase(enter_s, delay, offset, calibrated, gap)
                            first_watch = 1_000_000.0 - 1000 * enter_s + 1155
                            visible = 1_000_000.0 + offset > first_watch + 20
                            with self.subTest(enter_s=enter_s, delay=delay, offset=offset, calibrated=calibrated, gap=gap):
                                if visible:
                                    self.assertIsNone(refused)
                                if refused is None:
                                    self.assertLessEqual(abs(error), 20)
                                    self.assertLess(overrun, 0)      # no watch past the real press moment
                                else:
                                    self.assertEqual(refused, 'DIALOG_ZERO_UNCALIBRATED')

    def test_back_to_back_watches_without_the_zero_frame_never_calibrate(self):
        zero, footer = 1_000_000.0, 1_000_000.0 - 35
        # The zero between two watches: no tick at all (no inferred boundary).
        clock = CountdownClock()
        clock.add_watch(dialog_watch(zero, zero - 600, 594))
        clock.add_watch(dialog_watch(zero, zero + 1, 300))
        self.assertFalse(dialog_zero(clock, footer, footer, True)['calibrated'])
        # A baseline misread as 0分0秒 while it still shows 0分1秒.
        clock = CountdownClock()
        clock.add_watch(dialog_watch(zero, zero - 1630, 1525))
        clock.add_watch(dialog_watch(zero, zero - 90, 300, misread=(1, 0)))
        estimate = dialog_zero(clock, footer, footer, True)
        self.assertFalse(estimate['observed'])
        self.assertLessEqual(abs(estimate['zero_ms'] - zero), 17)
        # A 0 right after 0分2秒 (one second skipped) is no sight of the zero.
        clock = CountdownClock()
        clock.add_watch(dialog_watch(zero, zero - 1600, 1200, misread=(1, 0)))
        self.assertFalse(dialog_zero(clock, footer, footer, True)['calibrated'])

    def test_a_zero_after_an_unreadable_second_is_still_seen(self):
        # Review wf_3215e495-f2d: the native watch stops on it, Python must take it.
        zero, footer = 1_000_000.0, 1_000_000.0 - 107
        watch = dialog_watch(zero, zero - 1945, 3000)
        changes = [e for e in watch['events'] if e['kind'] == 'change']
        changes[0]['text'] = ''                           # the 2 -> 1 change unreadable
        watch, end = stop_at_zero(watch)
        self.assertIsNotNone(end)
        clock = CountdownClock()
        clock.add_watch(watch)
        estimate = dialog_zero(clock, footer, footer, True)
        self.assertTrue(estimate['observed'] and estimate['calibrated'])
        self.assertLessEqual(abs(estimate['zero_ms'] - zero), 17)

    def test_a_one_second_early_zero_is_bounded_by_the_frames_still_showing_one(self):
        # Review wf_3215e495-f2d: two misreads made an early group; the frames
        # showing 0分1秒 up to the real zero rule it out.
        zero = 1_000_000.0
        watch, end = stop_at_zero(dialog_watch(zero, zero - 900, 1500))
        bound = earliest_dialog_zero(zero - 1500, watch, zero + 300)
        self.assertGreater(bound, zero - 100)
        self.assertLessEqual(bound, zero)

    def test_a_tick_outside_the_watchlist_window_is_never_used(self):
        zero, clock = 1_000_000.0, CountdownClock()
        estimate = clock.add_watch(dialog_watch(zero, zero - 1600, 1200, misread=(1, 7)), window=(zero - 1100, zero + 300))
        self.assertTrue(any(t.get('outside_window') for t in clock.watches[0]['ticks']))
        self.assertIsNone(estimate['display_zero'])

    def test_a_misread_second_never_places_the_press(self):
        # Reviews wf_993b38d0-6b8 / wf_b0b8309e-39d: one misread line, on any
        # watch, presses at the true moment or not at all.
        for misread in ((1, 0), (1, 7), (2, 0), (0, 2), (1, 11), (0, 1), (1, 2), (2, 1)):
            for misread_watch in (0, 1, 2):
                for enter_s in (2, 3, 5):
                    for delay in (160, 300, 795, 1200):
                        for offset in range(-900, 301, 75):
                            for calibrated in (True, False):
                                refused, error, overrun = self.simulate_dialog_phase(
                                    enter_s, delay, offset, calibrated, 15, misread=misread, misread_watch=misread_watch)
                                with self.subTest(misread=misread, watch=misread_watch, enter_s=enter_s, delay=delay,
                                                  offset=offset, calibrated=calibrated):
                                    if refused is None:
                                        self.assertLessEqual(abs(error), 20)
                                        self.assertLess(overrun, 0)
                                    else:
                                        self.assertTrue(refused.startswith(('DIALOG_ZERO_UNCALIBRATED',
                                                                            'PRESS_WINDOW_MISSED')), refused)

    def test_an_unplaced_page_after_a_follow_watch_is_watched_again_while_time_allows(self):
        # Live buy_cycle03 2026-10-10: 'unknown' on the frame the button turned
        # to the price, 2 s before the entry; the attempt gave up.
        from run_purchase_timed_buy import PAGE_REREAD_MIN_LEAD_MS, PAGE_UNKNOWN_REREADS, page_reread_due
        unknown, listed = dict(page='unknown'), dict(page='watchlist_listings')
        self.assertTrue(page_reread_due(unknown, 0, 2000))
        self.assertTrue(page_reread_due(unknown, PAGE_UNKNOWN_REREADS - 1, PAGE_REREAD_MIN_LEAD_MS))
        self.assertFalse(page_reread_due(unknown, PAGE_UNKNOWN_REREADS, 5000))       # bounded
        self.assertFalse(page_reread_due(unknown, 0, PAGE_REREAD_MIN_LEAD_MS - 1))   # no time left: refused as before
        self.assertFalse(page_reread_due(listed, 0, 5000))
        self.assertFalse(page_reread_due(dict(page='skin_listings'), 0, 5000))       # a known other page: refused

    def test_a_button_change_counts_only_after_the_last_publicity_read(self):
        # Review wf_993b38d0-6b8: events before a re-read that still showed 公示中 are no proof.
        from run_purchase_timed_buy import button_change_since_publicity as since
        events = [dict(changed_px=2800)]
        self.assertTrue(since(False, events, ['price_ready']))
        self.assertTrue(since(False, events, ['unknown', 'price_ready']))
        self.assertFalse(since(False, events, ['publicity', 'price_ready']))     # the watch's final read
        self.assertFalse(since(False, events, ['price_ready', 'publicity']))     # a later re-read
        self.assertTrue(since(True, [], ['price_ready']))                        # earlier proof stands
        self.assertFalse(since(True, [], ['publicity']))

    def test_the_press_needs_calibration_a_read_line_and_the_exact_moment(self):
        good = dict(calibrated=True)
        self.assertIsNone(press_decision(good, 'countdown', 0, 3000, 3005, 3000))
        self.assertEqual(press_decision(dict(calibrated=False), 'countdown', 0, 3000, 3005, 3000), 'DIALOG_ZERO_UNCALIBRATED')
        self.assertEqual(press_decision(good, 'unread', 0, 3000, 3005, 3000), 'DIALOG_LINE_NOT_READ')
        self.assertEqual(press_decision(good, 'countdown', 0, 6000, 6000, 3000), 'PRESS_TOO_FAR')
        self.assertTrue(press_decision(good, 'countdown', 0, 3000, 3000 + PRESS_LATE_LIMIT_MS + 1, 3000).startswith('PRESS_WINDOW_MISSED'))
        self.assertTrue(press_decision(good, 'countdown', 0, 3000, 2999, 3000).startswith('PRESS_WINDOW_MISSED'))
        self.assertEqual(last_line_state(dialog_watch(5_000_000.0, 4_998_500.0, 1200)), 'countdown')


class EarlyPressProbeTests(unittest.TestCase):
    """User 2026-10-09 "再测一下小窗里按早了的提示": one press well before the
    unlock; review wf_8755a34e-3bd: calibrated against the watchlist clock,
    bounded on both sides, against a fresh line reading."""
    ZERO = 5_000_000.0

    def calibrate(self, first_start, lead=1500):
        """The probe's watches: the first one ends EARLY_FIRST_WATCH_SLACK_MS
        early, the second right before the press. Returns (zero estimate,
        line state, line seconds, end of the last watch)."""
        zero, clock = self.ZERO, CountdownClock()
        footer = zero - 107                       # the footer zero, calibrated
        first_end = zero - lead - LEAN_MARGIN_MS - EARLY_FIRST_WATCH_SLACK_MS
        clock.add_watch(dialog_watch(zero, first_start, first_end - first_start))
        estimate = dialog_zero(clock, footer - 107, footer, True)
        if not probe_zero_ready(estimate, True):
            return estimate, None, None, first_end
        last_end = estimate['zero_ms'] - lead - LEAN_MARGIN_MS
        second = dialog_watch(zero, first_end, last_end - first_end)
        clock.add_watch(second)
        return (dialog_zero(clock, footer - 107, footer, True), last_line_state(second), last_line_seconds(second),
                last_end)

    def test_an_entry_at_4_5_s_calibrates_and_presses_at_its_moment(self):
        for first_start in (self.ZERO - 3600, self.ZERO - 2600):   # two ticks, or one tick after a late entry
            with self.subTest(first_start=first_start):
                zero, state, seconds, end = self.calibrate(first_start)
                self.assertTrue(probe_zero_ready(zero, True) and not zero['observed'])
                self.assertLessEqual(abs(zero['zero_ms'] - self.ZERO), 17)
                # 0分0秒 shows from the zero, so 1.54 s before it the line reads 0分2秒.
                self.assertEqual((state, seconds), ('countdown', 2))
                press = zero['zero_ms'] - 1500 + 3
                self.assertIsNone(early_press_decision(zero, state, seconds, press, 1500, footer_calibrated=True,
                                                       watch_end_ms=end, frame_ms=end - 12, max_gap_ms=17))
                self.assertGreater(self.ZERO + 985 - press, 2400)          # the unlock is still far

    def test_no_early_press_on_any_doubt(self):
        good = dict(calibrated=True, observed=False, zero_ms=10_000.0, source='dialog', agrees_with_footer=True,
                    ticks_used=2)
        ok = dict(footer_calibrated=True, watch_end_ms=8_460.0, frame_ms=8_448.0, max_gap_ms=17)

        def decide(zero=good, state='countdown', seconds=2, now=8_503.0, lead=1500, **kw):
            return early_press_decision(zero, state, seconds, now, lead, **dict(ok, **kw))
        self.assertIsNone(decide())
        self.assertEqual(decide(footer_calibrated=False), 'FOOTER_UNCALIBRATED')
        self.assertEqual(decide(dict(good, agrees_with_footer=False)), 'DIALOG_ZERO_UNCALIBRATED')
        self.assertEqual(decide(dict(good, source='prior')), 'DIALOG_ZERO_UNCALIBRATED')
        self.assertEqual(decide(dict(good, calibrated=False, ticks_used=0)), 'DIALOG_ZERO_UNCALIBRATED')
        self.assertIsNone(decide(dict(good, calibrated=False, ticks_used=1)))   # one tick agreeing with the footer
        self.assertEqual(decide(dict(good, observed=True)), 'DIALOG_ZERO_ALREADY_SEEN')
        for state, seconds in (('unlocked', None), ('unread', None), ('countdown', None)):
            self.assertTrue(decide(state=state, seconds=seconds).startswith('DIALOG_LINE:'))
        # 0分1秒 (a zero a whole second late) or a misread digit where 0分2秒 is due.
        for seconds in (1, 3, 482):
            self.assertTrue(decide(seconds=seconds).startswith('DIALOG_LINE_DISAGREES'))
        self.assertTrue(decide(watch_end_ms=None).startswith('LINE_READ_STALE'))
        self.assertTrue(decide(watch_end_ms=8_503.0 - EARLY_LINE_MAX_AGE_MS - 1).startswith('LINE_READ_STALE'))
        # Bounded on both sides of the planned moment zero - lead.
        self.assertTrue(decide(now=8_499.0).startswith('PRESS_WINDOW_MISSED'))
        self.assertTrue(decide(now=8_500.0 + PRESS_LATE_LIMIT_MS + 1).startswith('PRESS_WINDOW_MISSED'))
        # The shortest lead still presses (review: it never could), never closer than the floor.
        self.assertIsNone(decide(now=8_805.0, lead=EARLY_PRESS_MIN_LEAD_MS, watch_end_ms=8_760.0, frame_ms=8_748.0))
        # A hung game (no fresh frame, or a long gap) is never pressed into.
        for kw in (dict(frame_ms=8_460.0 - EARLY_FRAME_MAX_AGE_MS - 1), dict(frame_ms=None),
                   dict(max_gap_ms=EARLY_MAX_FRAME_GAP_MS + 1), dict(max_gap_ms=None), dict(frame_ms=8_470.0)):
            self.assertTrue(decide(**kw).startswith('FRAMES_STALE'), kw)
        self.assertGreaterEqual(10_000.0 - (8_800.0 + PRESS_LATE_LIMIT_MS), EARLY_PRESS_MIN_AHEAD_MS)

    def test_the_probe_is_never_a_purchase(self):
        with self.assertRaisesRegex(ValueError, 'EARLY_PROBE_NEVER_BUYS'):
            InputBudget(buy=True, early=True)
        probe = InputBudget(buy=False, early=True)
        with self.assertRaisesRegex(ValueError, 'CONFIRM_NOT_ALLOWED'):
            probe.confirm(DIALOG_BUY_POINT)          # the pointer is not on the button yet
        probe.hover(DIALOG_BUY_POINT)
        probe.confirm(DIALOG_BUY_POINT)
        with self.assertRaisesRegex(ValueError, 'CONFIRM_NOT_ALLOWED'):
            probe.confirm(DIALOG_BUY_POINT)          # one press only
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'config.json'
            path.write_text(json.dumps(dict(run_settings=dict(purchaseDelayMs=830, enterBeforeSeconds=3))), 'utf-8')
            self.assertIsNone(settings_from(path, buy=True).early_press_ms)      # the F2 cycle's settings
            self.assertEqual(settings_from(path, buy=False, enter_at=4.5, early_press_ms=1500).early_press_ms, 1500)
            for kw, code in ((dict(buy=True, enter_at=4.5), 'NEVER_BUYS'),
                             (dict(buy=False, enter_at=4.5, cleanup_only=True), 'NEVER_BUYS'),
                             (dict(buy=False, enter_at=3.9), 'ENTER_TOO_LATE')):
                with self.subTest(kw=kw), self.assertRaisesRegex(ValueError, code):
                    settings_from(path, early_press_ms=1500, **kw)
            for lead in (EARLY_PRESS_MIN_LEAD_MS - 1, EARLY_PRESS_MAX_LEAD_MS + 1, 1500.0):
                with self.subTest(lead=lead), self.assertRaisesRegex(ValueError, 'EARLY_PROBE_LEAD'):
                    settings_from(path, buy=False, enter_at=5, early_press_ms=lead)


class HeadSwitchTests(unittest.TestCase):
    """User 2026-10-10: the right panel is followed, whichever listing it shows."""

    def test_another_listing_in_the_panel_is_noticed(self):
        from run_purchase_timed_buy import HEAD_SWITCH_JUMP_S, head_switched
        a = dict(product_title='AUG突击步枪-黑银先锋', selected_detail_precise='0.336411')
        b = dict(product_title='AUG突击步枪-黑银先锋', selected_detail_precise='0.289664')
        unread = dict(product_title='AUG突击步枪-黑银先锋', selected_detail_precise='')
        at = lambda seconds: dict(countdown_seconds=seconds)
        self.assertFalse(head_switched(a, a, at(30), 30.4))
        self.assertTrue(head_switched(a, b, at(30), 30.4))                 # another wear
        self.assertFalse(head_switched(a, unread, at(30), 30.4))           # an unread wear is no evidence
        # live early02: 32 -> 20 within one watch
        self.assertTrue(head_switched(a, unread, at(20), 31.0))
        self.assertFalse(head_switched(a, unread, at(29), 29.0 + HEAD_SWITCH_JUMP_S))
        self.assertFalse(head_switched(a, a, at(None), 30.0))


class SwitchEvidenceTests(unittest.TestCase):
    """Review wf_9d655fbd-4e6: a jump alone needs the watch's own verified
    ticks; buying only from 我的关注 or the very listing followed there."""

    def watch_and_after(self, zero, start=1_000_000.0, duration=9000, read_as=None):
        from purchase_clock import line_state
        watch = make_watch(zero, start, duration)
        shown = line_state(watch['events'][-1]['text'])[1]
        after = dict(countdown_seconds=shown if read_as is None else read_as,
                     source_frame=dict(source_mono_ms=watch['last_source_mono_ms']))
        return watch, after

    def test_a_misread_is_no_switch_and_a_real_one_is(self):
        from run_purchase_timed_buy import switch_evidence
        zero = 1_040_000.0
        a = dict(product_title='AUG突击步枪-黑银先锋', selected_detail_precise='0.336411')
        b = dict(a, selected_detail_precise='0.289664')
        watch, after = self.watch_and_after(zero)
        self.assertEqual(switch_evidence(a, a, after, watch, zero), (False, False))
        watch, after = self.watch_and_after(zero, read_as=after['countdown_seconds'] - 12)   # one full-frame misread
        self.assertEqual(switch_evidence(a, a, after, watch, zero), (False, False))
        watch, after = self.watch_and_after(zero - 12_000)                                   # the panel's listing changed
        self.assertEqual(switch_evidence(a, dict(a, selected_detail_precise=''), after, watch, zero), (False, True))
        watch, after = self.watch_and_after(zero)
        self.assertEqual(switch_evidence(a, b, after, watch, zero), (True, False))
        # The native watch ended on a jump of its line and the final full-frame
        # reading agrees: switched at once, even when that short watch's own
        # ticks cannot verify anything (user 2026-10-10: selection changed).
        watch, after = self.watch_and_after(zero, duration=400, read_as=200)
        watch['ended_by'] = 'jump'
        self.assertEqual(switch_evidence(a, dict(a, selected_detail_precise=''), after, watch, zero), (False, True))
        watch['ended_by'] = 'time'
        self.assertEqual(switch_evidence(a, dict(a, selected_detail_precise=''), after, watch, zero), (False, False))
        # A listing past its notice selected: the line jumped to 剩余, the panel shows the price button.
        watch.update(ended_by='jump', jump_seen=dict(to=-2))
        expired = dict(after, countdown_seconds=None, button_state='price_button')
        self.assertEqual(switch_evidence(a, dict(a, selected_detail_precise=''), expired, watch, zero), (False, True))

    def test_the_dialog_is_opened_from_the_watchlist_only(self):
        from run_purchase_timed_buy import entry_page_problem
        a = dict(selected_detail_precise='0.336411')
        self.assertIsNone(entry_page_problem(dict(page='watchlist_listings'), a, dict(selected_detail_precise='')))
        self.assertIsNone(entry_page_problem(dict(page='skin_listings'), a, dict(a)))     # misread page, same listing
        for current in (dict(selected_detail_precise='0.289664'), dict(selected_detail_precise=''), {}):
            self.assertTrue(entry_page_problem(dict(page='skin_listings'), a, current).startswith('PAGE_NOT_WATCHLIST'))
        self.assertTrue(entry_page_problem(dict(page='skin_listings'), dict(selected_detail_precise=''), {})
                        .startswith('PAGE_NOT_WATCHLIST'))


class OutcomeTests(unittest.TestCase):
    def test_outcomes_from_the_screens_after_the_press(self):
        def screen(*kinds):
            return dict(signals=[dict(kind=k) for k in kinds])
        self.assertEqual(outcome_of([screen(), screen('success_text')]), 'bought')
        self.assertEqual(outcome_of([screen('lottery_pool')]), 'lottery_pool')
        self.assertEqual(outcome_of([screen('recharge_prompt')]), 'insufficient_balance')
        self.assertEqual(outcome_of([screen('purchase_dialog')]), 'unknown')


class InputBudgetTests(unittest.TestCase):
    def test_one_hover_and_at_most_one_press_only_when_buying(self):
        rehearsal = InputBudget(buy=False)
        rehearsal.hover(DIALOG_BUY_POINT)
        with self.assertRaisesRegex(ValueError, 'CONFIRM_NOT_ALLOWED'):
            rehearsal.confirm(DIALOG_BUY_POINT)
        buy = InputBudget(buy=True)
        with self.assertRaisesRegex(ValueError, 'CONFIRM_NOT_ALLOWED'):
            buy.confirm(DIALOG_BUY_POINT)
        buy.hover(DIALOG_BUY_POINT)
        with self.assertRaisesRegex(ValueError, 'HOVER_NOT_ALLOWED'):
            buy.hover(DIALOG_BUY_POINT)
        with self.assertRaisesRegex(ValueError, 'CONFIRM_NOT_ALLOWED'):
            buy.confirm([1626, 1000])
        buy.confirm(DIALOG_BUY_POINT)
        with self.assertRaisesRegex(ValueError, 'CONFIRM_NOT_ALLOWED'):
            buy.confirm(DIALOG_BUY_POINT)
        x, y, w, h = DIALOG_BUY_RECT
        self.assertTrue(x < DIALOG_BUY_POINT[0] < x + w and y < DIALOG_BUY_POINT[1] < y + h)


if __name__ == '__main__':
    unittest.main()
